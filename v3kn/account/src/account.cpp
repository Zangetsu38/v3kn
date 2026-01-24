// v3knr project
// Copyright (C) 2026 Vita3K team

#include "account/account.h"
#include "friend/friend.h"
#include "utils/utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

static std::string generate_account_id(const time_t timestamp) {
    uint64_t ts = static_cast<uint64_t>(timestamp);
    uint64_t randomPart = ((uint64_t)rand() << 32) ^ rand();
    return "8" + std::to_string((ts << 32) | (randomPart & 0xFFFFFFFF));
}

void migrate_users_npid_to_account_id() {
    {
        std::lock_guard<std::mutex> lock(account_mutex);
        json db = load_users();
        if (!db.contains("users") || !db["users"].is_object())
            return;

        if (db.contains("tokens"))
            db.erase("tokens");

        auto &users = db["users"];

        bool modified = false;
        for (auto it = users.begin(); it != users.end();) {
            auto next = std::next(it);

            std::string old_npid = it.key();
            json user = it.value();

            const std::string online_id = user.value("online_id", "");
            if (online_id.empty()) {
                const auto created_at = user["created_at"].get<uint64_t>();
                std::string new_account_id = generate_account_id(created_at);

                user["online_id"] = old_npid;
                user["online_ids"] = json::array({ old_npid });

                users.erase(it);

                users[new_account_id] = user;
                modified = true;
                log("Migrated user from NPID " + old_npid + " to account ID " + new_account_id);
            } else
                log("User already has online ID " + online_id + ", skipping migration");

            it = next;
        }

        save_users(db);
    }
}

static std::optional<UserAccount> find_existing_online_id_case_insensitive(const json &users, const std::string &online_id) {
    const std::string needle = lowercase_online_id(online_id);

    for (const auto &[account_id, user] : users.items()) {
        if (!user.contains("online_ids") || !user["online_ids"].is_array())
            continue;

        for (const auto &id : user["online_ids"]) {
            const std::string existing = id.get<std::string>();
            if (lowercase_online_id(existing) == needle) {
                const std::string current_online_id = user["online_id"].get<std::string>();
                return UserAccount{ account_id, current_online_id };
            }
        }
    }

    return std::nullopt;
}

void update_profile_timestamp(const std::string &online_id) {
    json profile = load_profile(online_id);

    // Update the timestamp
    profile["last_updated_activity"] = static_cast<uint64_t>(std::time(0));

    save_profile(online_id, profile);
}

void register_account_endpoints(httplib::Server &server) {
    server.Get("/v3kn/check", handle_check_connection);
    server.Get("/v3kn/quota", handle_get_quota);
    server.Post("/v3kn/create", handle_create_account);
    server.Post("/v3kn/delete", handle_delete_account);
    server.Post("/v3kn/login", handle_login);
    server.Post("/v3kn/change_online_id", handle_change_online_id);
    server.Post("/v3kn/change_password", handle_change_password);
    server.Post("/v3kn/change_about_me", handle_change_about_me);
    server.Post("/v3kn/avatar", handle_upload_avatar);
    server.Get("/v3kn/avatar", handle_get_avatar);
    server.Post("/v3kn/panel", handle_upload_panel);
    server.Get("/v3kn/panel", handle_get_panel);
}

static bool is_valid_online_id(const std::string &online_id, const std::string &request, std::string &err) {
    if (online_id.empty()) {
        log(request + " with missing online ID");
        err = "ERR:MissingOnlineID";
        return false;
    }

    static const std::regex valid_online_id("^[A-Za-z][A-Za-z0-9_-]{2,15}$");
    if (!std::regex_match(online_id, valid_online_id)) {
        log(request + " with invalid online ID: " + online_id);
        err = "ERR:InvalidOnlineID";
        return false;
    }

    return true;
}

void handle_check_connection(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "connection check", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    update_last_activity(req, account_id);

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();
    auto &user = db["users"][account_id];
    const uint64_t created_at = user["created_at"];
    const uint64_t used = user["quota_used"];
    const uint64_t total = DEFAULT_QUOTA_TOTAL;

    std::string user_agent = req.get_header_value("User-Agent");
    if (user_agent.empty()) {
        user_agent = "Unknown";
    }

    if (!is_valid_online_id(online_id, "connection check", err)) {
        const auto sanitize_online_id = [](const std::string &online_id) {
            std::string out;
            out.reserve(online_id.size());

            for (char c : online_id) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    if (!out.empty() || (std::isalpha(static_cast<unsigned char>(c)))) // Online ID cannot start with non-alphabetic character, skip it
                        out.push_back(c);
                }
            }

            if (out.size() < 3)
                out = "User" + std::to_string(rand() % 100000);

            if (out.size() > 16)
                out.resize(16);

            return out;
        };

        std::string fixed_online_id = sanitize_online_id(online_id);
        while (find_existing_online_id_case_insensitive(db["users"], fixed_online_id)) {
            // Sufix based on the timestamp (max 3 digits)
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::string timestamp_str = "-" + std::to_string(ms % 10000);

            // How many characters need to be removed?
            const auto dif = static_cast<int32_t>(fixed_online_id.size() + timestamp_str.size()) - 16;

            // Remove characters from the end of the online ID until it fits with the timestamp suffix
            if (dif > 0)
                fixed_online_id.erase(fixed_online_id.size() - dif, dif);

            // Append the timestamp suffix
            fixed_online_id += timestamp_str;
        }

        auto &ids = user["online_ids"];
        // Remove the old invalid online ID from the list of online IDs
        ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const json &id) {
            return id.get<std::string>() == online_id;
        }),
            ids.end());

        user["online_ids"].push_back(fixed_online_id);
        user["online_id"] = fixed_online_id;
        save_users(db);
        const fs::path users_path = fs::path("v3kn") / "Users";
        if (fs::exists(users_path / online_id)) {
            fs::rename(users_path / online_id, users_path / fixed_online_id);
        } else {
            log("Online ID directory for " + online_id + " does not exist, cannot rename to " + fixed_online_id);
        }

        log("Updated online ID from " + online_id + " to " + fixed_online_id + " due to invalid characters");
        {
            std::lock_guard<std::mutex> cache_lock(online_id_cache_mutex);
            online_id_cache.erase(online_id);
            online_id_cache[fixed_online_id] = account_id;
        }
        {
            std::lock_guard<std::mutex> cache_lock(account_id_cache_mutex);
            account_id_cache[account_id] = fixed_online_id;
        }
        json profile = load_profile(fixed_online_id);
        profile["online_id"] = fixed_online_id;
        save_profile(fixed_online_id, profile);
        res.set_content(err + ":" + "NewOnlineID:" + fixed_online_id, "text/plain");
        return;
    }

    log("Connection check OK for online ID " + online_id + " from " + user_agent);
    res.set_content("OK:Connected:" + std::to_string(created_at) + ":" + std::to_string(used) + ":" + std::to_string(total), "text/plain");
}

void handle_get_quota(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "quota retrieval", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }
    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    json db = load_users();
    auto &user = db["users"][account_id];
    const uint64_t used = user["quota_used"];
    const uint64_t total = DEFAULT_QUOTA_TOTAL;

    log("Quota for online ID " + online_id + ": " + std::to_string(used) + " / " + std::to_string(total));
    update_last_activity(req, account_id);

    res.set_content("OK:" + std::to_string(used) + ":" + std::to_string(total), "text/plain");
}

void handle_create_account(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    const std::string online_id = trim_online_id(req.get_param_value("online_id"));

    std::string err;
    if (!is_valid_online_id(online_id, "account creation", err)) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string base64_password = req.get_param_value("password");
    if (base64_password.empty()) {
        log("Missing password attempt for online ID " + online_id);
        res.set_content("ERR:MissingPassword", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();
    const auto existing_account = find_existing_online_id_case_insensitive(db["users"], online_id);
    if (existing_account) {
        log("Account creation attempt for existing online ID " + online_id);
        res.set_content("ERR:UserExists", "text/plain");
        return;
    }

    const std::string password = base64_decode(base64_password);
    const std::vector<unsigned char> salt = generate_salt();
    const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
    const std::string token = generate_token();

    const std::time_t current_time = std::time(0);
    const auto account_id = generate_account_id(current_time);
    auto &user = db["users"][account_id];
    user["created_at"] = current_time;
    user["last_activity"] = current_time;
    user["last_login"] = current_time;
    user["quota_used"] = 0;
    user["online_ids"] = json::array({ online_id });
    user["online_id"] = online_id;
    user["password"] = base64_encode(std::string(server_hash.begin(), server_hash.end()));
    user["salt"] = base64_encode(std::string(salt.begin(), salt.end()));
    user["token"] = token;

    update_remote_addr(req, user);
    db["tokens"][token] = account_id;
    save_users(db);

    // Update the account_id -> online_id cache with the new account ID
    {
        std::lock_guard<std::mutex> cache_lock(account_id_cache_mutex);
        account_id_cache[account_id] = online_id;
    }

    // Update the online_id -> account_id cache with the new online ID
    {
        std::lock_guard<std::mutex> cache_lock(online_id_cache_mutex);
        online_id_cache[online_id] = account_id;
    }

    // Update the token -> account_id cache with the new token
    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache[token] = account_id;
    }

    const auto account_path = fs::path("v3kn") / "Users" / online_id;
    fs::create_directories(account_path / "savedata");
    fs::create_directories(account_path / "trophy");

    // Create a minimal profile for the new account
    json profile = load_profile(online_id);
    save_profile(online_id, profile);

    log("Created account for Account ID " + account_id + " with online ID " + online_id);
    res.set_content("OK:" + token, "text/plain");
}

void handle_delete_account(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "account deletion", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    const std::string base64_password = req.get_param_value("password");
    if (base64_password.empty()) {
        log("Missing password on account deletion attempt for online ID " + online_id);
        res.set_content("ERR:MissingPassword", "text/plain");
        return;
    }

    const std::string password = base64_decode(base64_password);

    std::lock_guard<std::mutex> lock(account_mutex);
    json db = load_users();
    auto &user = db["users"][account_id];

    const std::string base64_salt = user["salt"];
    const std::string salt_str = base64_decode(base64_salt);
    const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

    const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
    const std::string base64_server_hash = base64_encode(std::string(server_hash.begin(), server_hash.end()));

    if (user["password"] != base64_server_hash) {
        log("Invalid password on account deletion attempt for online ID " + online_id);
        res.set_content("ERR:InvalidPassword", "text/plain");
        return;
    }

    // Remove all online IDs of the user from the online ID cache
    for (const auto &id : user["online_ids"]) {
        const std::string existing_online_id = id.get<std::string>();
        {
            std::lock_guard<std::mutex> cache_lock(online_id_cache_mutex);
            online_id_cache.erase(existing_online_id);
        }
    }

    // Remove the user's account ID from the account ID cache
    {
        std::lock_guard<std::mutex> cache_lock(account_id_cache_mutex);
        account_id_cache.erase(account_id);
    }

    // Remove the user's token from the token cache
    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache.erase(user["token"]);
    }

    db["users"].erase(account_id);
    save_users(db);

    fs::remove_all("v3kn/Users/" + online_id);
    log("Deleting account for online ID " + online_id);
    res.set_content("OK:UserDeleted", "text/plain");
}

void handle_login(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    const std::string login_online_id = trim_online_id(req.get_param_value("online_id"));
    if (login_online_id.empty()) {
        log("Missing online ID on login attempt");
        res.set_content("ERR:MissingOnlineID", "text/plain");
        return;
    }

    const std::string base64_password = req.get_param_value("password");
    if (base64_password.empty()) {
        log("Missing password on login attempt for online ID " + login_online_id);
        res.set_content("ERR:MissingPassword", "text/plain");
        return;
    }

    const std::string password = base64_decode(base64_password);

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    const auto account = find_existing_online_id_case_insensitive(db["users"], login_online_id);
    if (!account) {
        log("Login attempt for non-existing online ID " + login_online_id);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    const auto account_id = account->account_id;
    const auto online_id = account->online_id;

    auto &user = db["users"][account_id];
    const std::string base64_salt = user["salt"];
    const std::string salt_str = base64_decode(base64_salt);
    const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

    const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
    const std::string base64_server_hash = base64_encode(std::string(server_hash.begin(), server_hash.end()));

    if (user["password"] != base64_server_hash) {
        log("Invalid password on login attempt for online ID " + online_id);
        res.set_content("ERR:InvalidPassword", "text/plain");
        return;
    }

    const std::string token = user["token"];
    const uint64_t used = user["quota_used"];

    user["last_login"] = std::time(0);
    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);
    save_users(db);

    json profile = load_profile(online_id);
    save_profile(online_id, profile);

    log("User " + online_id + " logged in.");
    res.set_content("OK:" + online_id + ":" + token, "text/plain");
}

void handle_change_online_id(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "online ID change", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const auto account_id = account->account_id;
    const auto online_id = account->online_id;

    const std::string new_online_id = trim_online_id(req.get_param_value("new_online_id"));
    if (new_online_id.empty()) {
        log("Missing new online ID on online ID change attempt for online ID " + online_id);
        res.set_content("ERR:MissingOnlineID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();
    const auto existing_online_id = find_existing_online_id_case_insensitive(db["users"], new_online_id);
    if (existing_online_id) {
        log("Online ID change attempt to existing online ID " + new_online_id + " by user " + online_id);
        res.set_content("ERR:UserExists", "text/plain");
        return;
    }

    auto &user = db["users"][account_id];
    user["online_ids"].push_back(new_online_id);
    user["online_id"] = new_online_id;
    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);
    save_users(db);

    // Update the account_id -> online_id cache with the new online ID
    {
        std::lock_guard<std::mutex> cache_lock(account_id_cache_mutex);
        account_id_cache[account_id] = new_online_id;
    }

    // Update the online_id -> account_id cache with the new online ID
    {
        std::lock_guard<std::mutex> cache_lock(online_id_cache_mutex);
        online_id_cache[new_online_id] = account_id;
    }

    fs::rename("v3kn/Users/" + online_id, "v3kn/Users/" + new_online_id);

    // Update the profile with the new online ID
    json profile = load_profile(new_online_id);
    profile["online_id"] = new_online_id;
    save_profile(new_online_id, profile);

    log("User " + online_id + " changed online ID to " + new_online_id);
    res.set_content("OK:OnlineIDChanged", "text/plain");
}

void handle_change_password(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "password change", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    const std::string base64_old_password = req.get_param_value("old_password");
    if (base64_old_password.empty()) {
        log("Missing old password on password change attempt for online ID " + online_id);
        res.set_content("ERR:MissingOldPassword", "text/plain");
        return;
    }

    const std::string base64_new_password = req.get_param_value("new_password");
    if (base64_new_password.empty()) {
        log("Missing new password on password change attempt for online ID " + online_id);
        res.set_content("ERR:MissingNewPassword", "text/plain");
        return;
    }

    if (base64_old_password == base64_new_password) {
        log("Same password provided on password change attempt for online ID " + online_id);
        res.set_content("ERR:SamePassword", "text/plain");
        return;
    }

    const std::string new_password = base64_decode(base64_new_password);

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();
    auto &user = db["users"][account_id];

    const std::string base64_salt = user["salt"];
    const std::string salt_str = base64_decode(base64_salt);
    const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

    const std::string old_password = base64_decode(base64_old_password);
    const std::vector<unsigned char> old_server_hash = compute_server_hash(old_password, salt);
    const std::string base64_old_server_hash = base64_encode(std::string(old_server_hash.begin(), old_server_hash.end()));

    if (user["password"] != base64_old_server_hash) {
        log("Invalid old password on password change attempt for online ID " + online_id);
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

    db["tokens"][new_token] = account_id;
    save_users(db);

    {
        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
        token_cache[new_token] = account_id;
    }

    log("User " + online_id + " changed their password (new token generated).");
    res.set_content("OK:" + new_token, "text/plain");
}

void handle_change_about_me(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const auto account = get_valid_account(req, "about me change", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    const std::string about_me = req.get_param_value("about_me");
    if (about_me.empty()) {
        log("Missing About Me on about me change attempt for online ID " + online_id);
        res.set_content("ERR:MissingAboutMe", "text/plain");
        return;
    }

    if (about_me.size() > 21) {
        log("About Me too long on about me change attempt for online ID " + online_id);
        res.set_content("ERR:AboutMeTooLong", "text/plain");
        return;
    }

    json profile = load_profile(online_id);
    profile["about_me"] = about_me;
    save_profile(online_id, profile);

    update_last_activity(req, account_id);

    log("User " + online_id + " changed their About Me.");
    res.set_content("OK:AboutMeChanged", "text/plain");
}

void handle_upload_avatar(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "avatar upload", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    if (!req.form.has_file("file")) {
        log("Missing file on avatar upload for online ID " + online_id);
        res.set_content("ERR:MissingFile", "text/plain");
        return;
    }

    const auto &file = req.form.get_file("file");
    if (file.content.empty()) {
        log("Empty file on avatar upload for online ID " + online_id);
        res.set_content("ERR:EmptyFile", "text/plain");
        return;
    }

    // Max 2MB
    if (file.content.size() > 2 * 1024 * 1024) {
        log("Avatar too large for online ID " + online_id + " (" + std::to_string(file.content.size()) + " bytes)");
        res.set_content("ERR:FileTooLarge", "text/plain");
        return;
    }

    // Verify PNG signature and dimensions (max 128x128)
    const auto &data = file.content;
    const uint8_t png_sig[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (data.size() < 24 || std::memcmp(data.data(), png_sig, 8) != 0) {
        log("Invalid PNG file on avatar upload for online ID " + online_id);
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
        log("Avatar dimensions too large for online ID " + online_id + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
        res.set_content("ERR:DimensionsTooLarge", "text/plain");
        return;
    }

    const fs::path avatar_path = fs::path("v3kn") / "Users" / online_id / "Avatar.png";
    fs::create_directories(avatar_path.parent_path());

    std::ofstream out(avatar_path, std::ios::binary);
    out << file.content;

    update_last_activity(req, account_id);

    log("Avatar uploaded for online ID " + online_id + " (" + std::to_string(file.content.size()) + " bytes)");
    res.set_content("OK:AvatarUploaded", "text/plain");
}

void handle_get_avatar(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "avatar download", err);
    if (!account) {
        log("Missing or invalid token on avatar download attempt");
        res.set_content(err, "text/plain");
        return;
    }
    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const std::string &target_online_id = req.get_param_value("target_online_id");
    if (target_online_id.empty()) {
        log("Missing target online ID on avatar download attempt by online ID " + online_id);
        res.set_content("ERR:MissingTargetOnlineID", "text/plain");
        return;
    }

    const fs::path avatar_path = fs::path("v3kn") / "Users" / target_online_id / "Avatar.png";
    if (!fs::exists(avatar_path)) {
        log("Avatar not found for online ID " + target_online_id + " requested by online ID " + online_id);
        res.set_content("ERR:NoAvatar", "text/plain");
        return;
    }

    std::ifstream f(avatar_path, std::ios::binary);
    std::stringstream buffer;
    buffer << f.rdbuf();

    update_last_activity(req, account_id);

    res.set_content(buffer.str(), "image/png");
}

void handle_upload_panel(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "panel upload", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    if (!req.form.has_file("file")) {
        log("Missing file on panel upload for online ID " + online_id);
        res.set_content("ERR:MissingFile", "text/plain");
        return;
    }

    const auto &file = req.form.get_file("file");
    if (file.content.empty()) {
        log("Empty file on panel upload for online ID " + online_id);
        res.set_content("ERR:EmptyFile", "text/plain");
        return;
    }

    // Max 2MB
    if (file.content.size() > 2 * 1024 * 1024) {
        log("Panel too large for online ID " + online_id + " (" + std::to_string(file.content.size()) + " bytes)");
        res.set_content("ERR:FileTooLarge", "text/plain");
        return;
    }

    // Verify PNG signature
    const auto &data = file.content;
    const uint8_t png_sig[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (data.size() < 24 || std::memcmp(data.data(), png_sig, 8) != 0) {
        log("Invalid PNG file on panel upload for online ID " + online_id);
        res.set_content("ERR:InvalidPNG", "text/plain");
        return;
    }

    // Width at offset 16, height at offset 20 (big-endian uint32)
    const auto read_be32 = [&](size_t offset) -> uint32_t {
        return (static_cast<uint8_t>(data[offset]) << 24) | (static_cast<uint8_t>(data[offset + 1]) << 16) | (static_cast<uint8_t>(data[offset + 2]) << 8) | static_cast<uint8_t>(data[offset + 3]);
    };
    const uint32_t width = read_be32(16);
    const uint32_t height = read_be32(20);

    if (width != 400 || height != 80) {
        log("Panel dimensions invalid for online ID " + online_id + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
        res.set_content("ERR:InvalidDimensions", "text/plain");
        return;
    }

    const fs::path panel_path = fs::path("v3kn") / "Users" / online_id / "Panel.png";
    fs::create_directories(panel_path.parent_path());

    std::ofstream out(panel_path, std::ios::binary);
    out << file.content;

    update_last_activity(req, account_id);

    log("Panel uploaded for online ID " + online_id + " (" + std::to_string(file.content.size()) + " bytes)");
    res.set_content("OK:PanelUploaded", "text/plain");
}

void handle_get_panel(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "panel download", err);
    if (!account) {
        log("Missing or invalid token on panel download attempt");
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const std::string &target_online_id = req.get_param_value("target_online_id");
    if (target_online_id.empty()) {
        log("Missing target online ID on panel download attempt by online ID " + online_id);
        res.set_content("ERR:MissingTargetOnlineID", "text/plain");
        return;
    }

    const fs::path panel_path = fs::path("v3kn") / "Users" / target_online_id / "Panel.png";
    if (!fs::exists(panel_path)) {
        log("Panel not found for online ID " + target_online_id + " requested by online_id " + online_id);
        res.set_content("ERR:NoPanel", "text/plain");
        return;
    }

    std::ifstream f(panel_path, std::ios::binary);
    std::stringstream buffer;
    buffer << f.rdbuf();

    update_last_activity(req, account_id);

    res.set_content(buffer.str(), "image/png");
}
