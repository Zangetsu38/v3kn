// v3knr project
// Copyright (C) 2026 Vita3K team

#include "account/account.h"
#include "friend/friend.h"
#include "utils/utils.h"

#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

void register_account_endpoints(httplib::Server &server) {
    server.Get("/v3kn/check", handle_check_connection);
    server.Get("/v3kn/quota", handle_get_quota);
    server.Post("/v3kn/create", handle_create_account);
    server.Post("/v3kn/delete", handle_delete_account);
    server.Post("/v3kn/login", handle_login);
    server.Post("/v3kn/change_npid", handle_change_npid);
    server.Post("/v3kn/change_password", handle_change_password);
    server.Post("/v3kn/avatar", handle_upload_avatar);
    server.Get("/v3kn/avatar", handle_get_avatar);
}

void handle_check_connection(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "check connection", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    json db = load_users();
    auto &user = db["users"][npid];
    const uint64_t created_at = user["created_at"];
    const uint64_t used = user["quota_used"];
    const uint64_t total = DEFAULT_QUOTA_TOTAL;

    update_last_activity(req, npid);

    std::string user_agent = req.get_header_value("User-Agent");
    if (user_agent.empty()) {
        user_agent = "Unknown";
    }
    log("Connection check OK for NPID " + npid + " from " + user_agent);
    res.set_content("OK:Connected:" + std::to_string(created_at) + ":" + std::to_string(used) + ":" + std::to_string(total), "text/plain");
}

void handle_get_quota(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "quota request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    json db = load_users();
    auto &user = db["users"][npid];
    const uint64_t used = user["quota_used"];
    const uint64_t total = DEFAULT_QUOTA_TOTAL;

    log("Quota for NPID " + npid + ": " + std::to_string(used) + " / " + std::to_string(total));
    update_last_activity(req, npid);

    res.set_content("OK:" + std::to_string(used) + ":" + std::to_string(total), "text/plain");
}

void handle_create_account(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    const std::string npid = trim_npid(req.get_param_value("npid"));
    if (npid.empty() || (npid.size() < 3) || (npid.size() > 16)) {
        log("Invalid NPID attempt");
        res.set_content("ERR:InvalidNPID", "text/plain");
        return;
    }

    const std::string base64_password = req.get_param_value("password");
    if (base64_password.empty()) {
        log("Missing password attempt for NPID " + npid);
        res.set_content("ERR:MissingPassword", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();
    if (db["users"].contains(npid)) {
        log("Account creation attempt for existing NPID " + npid);
        res.set_content("ERR:UserExists", "text/plain");
        return;
    }

    const std::string password = base64_decode(base64_password);
    const std::vector<unsigned char> salt = generate_salt();
    const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
    const std::string token = generate_token();

    auto &user = db["users"][npid];
    user["quota_used"] = 0;
    user["password"] = base64_encode(std::string(server_hash.begin(), server_hash.end()));
    user["salt"] = base64_encode(std::string(salt.begin(), salt.end()));
    user["token"] = token;
    user["created_at"] = std::time(0);
    user["last_login"] = std::time(0);
    user["last_activity"] = std::time(0);

    update_remote_addr(req, user);
    db["tokens"][token] = npid;
    save_users(db);

    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache[token] = npid;
    }

    const auto account_path = fs::path("v3kn") / "Users" / npid;
    fs::create_directories(account_path / "savedata");
    fs::create_directories(account_path / "trophy");

    log("Created account for NPID " + npid);
    res.set_content("OK:" + token, "text/plain");
}

void handle_delete_account(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "account deletion", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string base64_password = req.get_param_value("password");
    if (base64_password.empty()) {
        log("Missing password on account deletion attempt for NPID " + npid);
        res.set_content("ERR:MissingPassword", "text/plain");
        return;
    }

    const std::string password = base64_decode(base64_password);

    std::lock_guard<std::mutex> lock(account_mutex);
    json db = load_users();
    auto &user = db["users"][npid];

    const std::string base64_salt = user["salt"];
    const std::string salt_str = base64_decode(base64_salt);
    const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

    const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
    const std::string base64_server_hash = base64_encode(std::string(server_hash.begin(), server_hash.end()));

    if (user["password"] != base64_server_hash) {
        log("Invalid password on account deletion attempt for NPID " + npid);
        res.set_content("ERR:InvalidPassword", "text/plain");
        return;
    }

    db["tokens"].erase(user["token"]);
    db["users"].erase(npid);
    save_users(db);

    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache.erase(user["token"]);
    }

    fs::remove_all("v3kn/Users/" + npid);
    log("Deleting account for NPID " + npid);
    res.set_content("OK:UserDeleted", "text/plain");
}

void handle_login(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    const std::string npid = trim_npid(req.get_param_value("npid"));
    if (npid.empty()) {
        log("Missing NPID on login attempt");
        res.set_content("ERR:MissingNPID", "text/plain");
        return;
    }

    const std::string base64_password = req.get_param_value("password");
    if (base64_password.empty()) {
        log("Missing password on login attempt for NPID " + npid);
        res.set_content("ERR:MissingPassword", "text/plain");
        return;
    }

    const std::string password = base64_decode(base64_password);

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(npid)) {
        log("Login attempt for non-existing NPID " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    auto &user = db["users"][npid];

    const std::string base64_salt = user["salt"];
    const std::string salt_str = base64_decode(base64_salt);
    const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

    const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
    const std::string base64_server_hash = base64_encode(std::string(server_hash.begin(), server_hash.end()));

    if (user["password"] != base64_server_hash) {
        log("Invalid password on login attempt for NPID " + npid);
        res.set_content("ERR:InvalidPassword", "text/plain");
        return;
    }

    const std::string token = user["token"];
    const uint64_t used = user["quota_used"];

    user["last_login"] = std::time(0);
    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);
    save_users(db);

    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache[token] = npid;
    }

    log("User " + npid + " logged in.");
    res.set_content("OK:" + token + ":" + std::to_string(static_cast<uint64_t>(user["created_at"])) + ":" + std::to_string(used) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
}

void handle_change_npid(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "NPID change", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string new_npid = trim_npid(req.get_param_value("new_npid"));
    if (new_npid.empty()) {
        log("Missing new NPID on NPID change attempt for NPID " + npid);
        res.set_content("ERR:MissingNPID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();
    if (db["users"].contains(new_npid)) {
        log("NPID change attempt to existing NPID " + new_npid + " by user " + npid);
        res.set_content("ERR:UserExists", "text/plain");
        return;
    }

    db["users"][new_npid] = db["users"][npid];
    db["users"].erase(npid);

    auto &user = db["users"][new_npid];
    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);

    const std::string token = user["token"];
    db["tokens"][token] = new_npid;
    save_users(db);

    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache[token] = new_npid;
    }

    fs::rename("v3kn/Users/" + npid, "v3kn/Users/" + new_npid);
    log("User " + npid + " changed NPID to " + new_npid);
    res.set_content("OK:NPIDChanged", "text/plain");
}

void handle_change_password(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "password change", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string base64_old_password = req.get_param_value("old_password");
    if (base64_old_password.empty()) {
        log("Missing old password on password change attempt for NPID " + npid);
        res.set_content("ERR:MissingOldPassword", "text/plain");
        return;
    }

    const std::string base64_new_password = req.get_param_value("new_password");
    if (base64_new_password.empty()) {
        log("Missing new password on password change attempt for NPID " + npid);
        res.set_content("ERR:MissingNewPassword", "text/plain");
        return;
    }

    if (base64_old_password == base64_new_password) {
        log("Same password provided on password change attempt for NPID " + npid);
        res.set_content("ERR:SamePassword", "text/plain");
        return;
    }

    const std::string new_password = base64_decode(base64_new_password);

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();
    auto &user = db["users"][npid];

    const std::string base64_salt = user["salt"];
    const std::string salt_str = base64_decode(base64_salt);
    const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

    const std::string old_password = base64_decode(base64_old_password);
    const std::vector<unsigned char> old_server_hash = compute_server_hash(old_password, salt);
    const std::string base64_old_server_hash = base64_encode(std::string(old_server_hash.begin(), old_server_hash.end()));

    if (user["password"] != base64_old_server_hash) {
        log("Invalid old password on password change attempt for NPID " + npid);
        res.set_content("ERR:InvalidPassword", "text/plain");
        return;
    }

    const std::vector<unsigned char> new_salt = generate_salt();
    const std::vector<unsigned char> new_server_hash = compute_server_hash(new_password, new_salt);

    const std::string old_token = user["token"];
    db["tokens"].erase(old_token);
    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache.erase(old_token);
    }

    const std::string new_token = generate_token();
    user["password"] = base64_encode(std::string(new_server_hash.begin(), new_server_hash.end()));
    user["salt"] = base64_encode(std::string(new_salt.begin(), new_salt.end()));
    user["token"] = new_token;
    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);

    db["tokens"][new_token] = npid;
    save_users(db);

    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache[new_token] = npid;
    }

    log("User " + npid + " changed their password (new token generated).");
    res.set_content("OK:" + new_token, "text/plain");
}

void handle_upload_avatar(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "avatar upload", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    if (!req.form.has_file("file")) {
        log("Missing file on avatar upload for NPID " + npid);
        res.set_content("ERR:MissingFile", "text/plain");
        return;
    }

    const auto file = req.form.get_file("file");
    if (file.content.empty()) {
        log("Empty file on avatar upload for NPID " + npid);
        res.set_content("ERR:EmptyFile", "text/plain");
        return;
    }

    // Max 2MB
    if (file.content.size() > 2 * 1024 * 1024) {
        log("Avatar too large for NPID " + npid + " (" + std::to_string(file.content.size()) + " bytes)");
        res.set_content("ERR:FileTooLarge", "text/plain");
        return;
    }

    // Verify PNG signature and dimensions (max 128x128)
    const auto &data = file.content;
    const uint8_t png_sig[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (data.size() < 24 || std::memcmp(data.data(), png_sig, 8) != 0) {
        log("Invalid PNG file on avatar upload for NPID " + npid);
        res.set_content("ERR:InvalidPNG", "text/plain");
        return;
    }

    // Width at offset 16, height at offset 20 (big-endian uint32)
    const auto read_be32 = [&](size_t offset) -> uint32_t {
        return (static_cast<uint8_t>(data[offset]) << 24) | (static_cast<uint8_t>(data[offset + 1]) << 16) | (static_cast<uint8_t>(data[offset + 2]) << 8) | static_cast<uint8_t>(data[offset + 3]);
    };
    const uint32_t width = read_be32(16);
    const uint32_t height = read_be32(20);

    if (width > 128 || height > 128) {
        log("Avatar dimensions too large for NPID " + npid + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
        res.set_content("ERR:DimensionsTooLarge", "text/plain");
        return;
    }

    const fs::path avatar_path = fs::path("v3kn") / "Users" / npid / "Avatar.png";
    fs::create_directories(avatar_path.parent_path());

    std::ofstream out(avatar_path, std::ios::binary);
    out << file.content;

    update_last_activity(req, npid);
    notify_avatar_changed(npid);
    log("Avatar uploaded for NPID " + npid + " (" + std::to_string(file.content.size()) + " bytes)");
    res.set_content("OK:AvatarUploaded", "text/plain");
}

void handle_get_avatar(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "avatar download", err);
    if (npid.empty()) {
        log("Missing or invalid token on avatar download attempt");
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = req.get_param_value("npid");
    const std::string &lookup_npid = target_npid.empty() ? npid : trim_npid(target_npid);

    const fs::path avatar_path = fs::path("v3kn") / "Users" / lookup_npid / "Avatar.png";
    if (!fs::exists(avatar_path)) {
        log("Avatar not found for NPID " + lookup_npid + " requested by NPID " + npid);
        res.set_content("ERR:NoAvatar", "text/plain");
        return;
    }

    std::ifstream f(avatar_path, std::ios::binary);
    std::stringstream buffer;
    buffer << f.rdbuf();

    update_last_activity(req, npid);

    log("Avatar downloaded for NPID " + lookup_npid + " requested by NPID " + npid);

    res.set_content(buffer.str(), "image/png");
}
