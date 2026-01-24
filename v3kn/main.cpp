// v3knr project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <version/version.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <regex>
#include <unordered_map>

using json = nlohmann::json;

namespace fs = std::filesystem;

//  UTILITIES
static json load_users() {
    std::ifstream f("v3kn/users.json");
    if (!f.is_open())
        return json{ { "users", json::object() } };
    json db;
    f >> db;
    return db;
}

static void save_users(const json &db) {
    std::ofstream f("v3kn/users.json");
    f << db.dump(4);
}

static std::string generate_token() {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string token;
    token.resize(48);

    std::random_device rd; // obtain a random number from hardware
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, sizeof(chars) - 2); // -2 to exclude null terminator

    for (size_t i = 0; i < 48; ++i)
        token[i] = chars[dist(gen)];

    return token;
}

static std::vector<unsigned char> generate_salt(size_t length = 64) {
    std::vector<unsigned char> salt(length);

    // Using random_device for crypto-safe seeding
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned int> dist(0, 255);

    for (size_t i = 0; i < length; ++i) {
        salt[i] = static_cast<unsigned char>(dist(gen));
    }

    return salt;
}

static std::vector<unsigned char> compute_server_hash(const std::string &client_hash, const std::vector<unsigned char> &salt) {
    std::vector<unsigned char> hash(SHA256_DIGEST_LENGTH);

    // SHA3-256 (OpenSSL 3.0+)
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha3_256(), nullptr);
    EVP_DigestUpdate(ctx, client_hash.data(), client_hash.size());
    EVP_DigestUpdate(ctx, salt.data(), salt.size());
    EVP_DigestFinal_ex(ctx, hash.data(), nullptr);
    EVP_MD_CTX_free(ctx);

    return hash;
}

static std::string get_token_from_request(const httplib::Request &req) {
    if (!req.has_header("Authorization"))
        return "";

    std::string h = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";

    if (h.rfind(prefix, 0) == 0)
        return h.substr(prefix.size());

    return "";
}

static std::string get_metadata_from_request(const httplib::Request &req) {
    if (!req.has_header("Metadata"))
        return "";

    return req.get_header_value("Metadata");
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64_decode(const std::string &encoded) {
    std::string decoded;
    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (std::string(b64_table).find(c) == std::string::npos)
            break;
        val = (val << 6) + static_cast<int>(std::string(b64_table).find(c));
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return decoded;
}

static std::string trim_npid(std::string npid) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };

    // Trim left
    npid.erase(npid.begin(),
        std::find_if(npid.begin(), npid.end(),
            [&](unsigned char c) { return !is_space(c); }));

    // Trim right
    npid.erase(std::find_if(npid.rbegin(), npid.rend(),
                   [&](unsigned char c) { return !is_space(c); })
                   .base(),
        npid.end());

    return npid;
}

static std::string base64_encode(const std::string &input) {
    const unsigned char *data = reinterpret_cast<const unsigned char *>(input.data());
    size_t len = input.size();
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        unsigned int val = data[i] << 16;
        if (i + 1 < len)
            val |= data[i + 1] << 8;
        if (i + 2 < len)
            val |= data[i + 2];

        out.push_back(b64_table[(val >> 18) & 0x3F]);
        out.push_back(b64_table[(val >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? b64_table[(val >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? b64_table[val & 0x3F] : '=');
    }

    return out;
}

static std::mutex account_mutex;
static std::mutex request_mutex;
static std::unordered_map<std::string, std::string> token_cache; // token -> npid
static std::mutex token_cache_mutex;

static std::string get_npid_from_token(const std::string &token) {
    // Check cache only (no file I/O during requests)
    std::lock_guard<std::mutex> lock(token_cache_mutex);
    auto it = token_cache.find(token);
    if (it != token_cache.end()) {
        return it->second;
    }

    // Token not in cache = invalid token
    return "";
}

static std::string get_remote_addr(const httplib::Request &req) {
    const std::string ip = req.get_header_value("CF-Connecting-IP");
    return ip.empty() ? req.remote_addr : ip;
}

static void update_remote_addr(const httplib::Request &req, json &user) {
    const auto remote_addr = get_remote_addr(req);
    if (!user.contains("remote_addr") || !user["remote_addr"].is_array())
        user["remote_addr"] = json::array();

    if (std::none_of(user["remote_addr"].begin(), user["remote_addr"].end(),
            [&](const json &v) { return v.get<std::string>() == remote_addr; })) {
        user["remote_addr"].push_back(remote_addr);
    }
}

static void update_last_activity(const httplib::Request &req, const std::string &npid) {
    std::lock_guard<std::mutex> lock(account_mutex);
    json db = load_users();
    if (!db.contains("users") || !db["users"].contains(npid))
        return;

    auto &user = db["users"][npid];
    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);
    save_users(db);
}

static std::mutex log_mutex;

static void log(const std::string &msg) {
    std::lock_guard<std::mutex> lock(log_mutex);

    // 1. Timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now_c);
#else
    localtime_r(&now_c, &tm);
#endif

    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%d-%m-%Y %H:%M:%S", &tm);

    // 2. Build log message
    std::string full = std::string("[") + timebuf + "] " + msg;

    // 3. Console
    std::cout << full << std::endl;

    // Main log
    {
        std::ofstream root("v3kn.log", std::ios::app);
        root << full << "\n";
    }

    // 4. Build folder structure: logs/YYYY/MM/
    char yearbuf[8], monthbuf[8], daybuf[8];
    std::strftime(yearbuf, sizeof(yearbuf), "%Y", &tm);
    std::strftime(monthbuf, sizeof(monthbuf), "%m", &tm);
    std::strftime(daybuf, sizeof(daybuf), "%d", &tm);

    fs::path folder = fs::path("logs") / yearbuf / monthbuf;

    // Create directories if needed
    fs::create_directories(folder);

    // 5. File path: logs/YYYY/MM/DD.log
    fs::path filepath = folder / (std::string(daybuf) + ".log");

    // 6. Append to file
    std::ofstream f(filepath, std::ios::app);
    f << full << "\n";
}

static std::string get_valid_npid(const httplib::Request &req, const std::string &request, std::string &err) {
    // Get token from request
    const std::string token = get_token_from_request(req);
    if (token.empty()) {
        log("Missing token on request: " + request);
        err = "ERR:MissingToken";
        return {};
    }

    // Validate token and get NPID
    const std::string npid = get_npid_from_token(token);
    if (npid.empty()) {
        log("Invalid token on request: " + request);
        err = "ERR:InvalidToken";
        return {};
    }

    return npid;
}

static constexpr uint64_t DEFAULT_QUOTA_TOTAL = 50 * 1024 * 1024; // 50 MB

//  SERVER
int main() {
    auto seed = static_cast<unsigned int>(
        std::chrono::system_clock::now().time_since_epoch().count());
    srand(seed);
    httplib::Server v3kn;

    v3kn.new_task_queue = [] {
        return new httplib::ThreadPool(32);
    };

    v3kn.set_read_timeout(120, 0);
    v3kn.set_write_timeout(120, 0);
    v3kn.set_payload_max_length(100 * 1024 * 1024);
    v3kn.set_keep_alive_max_count(10000);
    v3kn.set_keep_alive_timeout(300);
    v3kn.set_tcp_nodelay(true);

    v3kn.set_logger([](const httplib::Request &req, const httplib::Response &) {
        const std::string cn = req.get_header_value("CF-IPCountry");
        const std::string country = cn.empty() ? "XX" : cn;
        const std::string ip = req.get_header_value("CF-Connecting-IP");
        const std::string remote_addr = ip.empty() ? req.remote_addr : ip;
        std::string msg = req.method + " " + req.path + " from [" + country + "] " + remote_addr + ":" + std::to_string(req.remote_port);

        if (req.has_header("User-Agent"))
            msg += "\n  UA: " + req.get_header_value("User-Agent");

        log(msg);
    });

    v3kn.Get("/", [&](const httplib::Request &req, httplib::Response &res) {
        std::string html = R"(
        <html>
            <head><title>v3kn</title></head>
            <body>
                <h1>v3kn server is running</h1>
                <p>Welcome to the Vita3K Network server!</p>
            </body>
        </html>
        )";

        res.set_content(html, "text/html");
    });

    // GET FAVICON
    v3kn.Get("/favicon.ico", [&](const httplib::Request &req, httplib::Response &res) {
        std::ifstream file("favicon.ico", std::ios::binary);
        if (!file) {
            res.status = 404;
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        res.set_content(buffer.str(), "image/x-icon");
    });

    // CHECK CONNECTION
    v3kn.Get("/v3kn/check", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> req_lock(request_mutex);

        std::string err;
        const std::string npid = get_valid_npid(req, "check connection", err);
        if (npid.empty()) {
            res.set_content(err, "text/plain");
            return;
        }

        update_last_activity(req, npid);

        log("Connection check OK for NPID " + npid);
        res.set_content("OK:Connected", "text/plain");
    });

    // GET QUOTA
    v3kn.Get("/v3kn/quota", [&](const httplib::Request &req, httplib::Response &res) {
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

        const auto msg = "Quota for NPID " + npid + ": " + std::to_string(used) + " / " + std::to_string(total);
        log(msg);

        update_last_activity(req, npid);

        res.set_content("OK:" + std::to_string(used) + ":" + std::to_string(total), "text/plain");
    });

    //  CREATE ACCOUNT
    v3kn.Post("/v3kn/create", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> req_lock(request_mutex);

        const std::string npid = trim_npid(req.get_param_value("npid"));
        if (npid.empty()) {
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

        // Decode password from base64
        const std::string password = base64_decode(base64_password);

        // Generate salt
        const std::vector<unsigned char> salt = generate_salt();

        // Compute server hash
        const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);

        // Generate token
        const std::string token = generate_token();

        // Store user
        auto &user = db["users"][npid];
        user["quota_used"] = 0;
        user["password"] = base64_encode(std::string(server_hash.begin(), server_hash.end()));
        user["salt"] = base64_encode(std::string(salt.begin(), salt.end()));
        user["token"] = token;
        user["created_at"] = std::time(0);
        user["last_login"] = std::time(0);
        user["last_activity"] = std::time(0);

        update_remote_addr(req, user);

        // Reverse lookup
        db["tokens"][token] = npid;

        save_users(db);

        // Update cache
        {
            std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
            token_cache[token] = npid;
        }

        const auto account_path = fs::path("v3kn") / npid;
        fs::create_directories(account_path / "savedata");
        fs::create_directories(account_path / "trophy");

        const auto msg = "Created account for NPID " + npid;
        log(msg);

        res.set_content("OK:" + token + ":" + std::to_string(std::time(0)) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
    });

    // Delete
    v3kn.Post("/v3kn/delete", [&](const httplib::Request &req, httplib::Response &res) {
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

        // Decode password from base64
        const std::string password = base64_decode(base64_password);

        std::lock_guard<std::mutex> lock(account_mutex);
        json db = load_users();
        auto &user = db["users"][npid];

        // Get the salt
        const std::string base64_salt = user["salt"];
        const std::string salt_str = base64_decode(base64_salt);
        const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

        // Compute the server hash
        const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
        const std::string base64_server_hash = base64_encode(std::string(server_hash.begin(), server_hash.end()));

        // Compare hashes
        if (user["password"] != base64_server_hash) {
            log("Invalid password on account deletion attempt for NPID " + npid);
            res.set_content("ERR:InvalidPassword", "text/plain");
            return;
        }

        // Delete user data
        db["tokens"].erase(user["token"]);
        db["users"].erase(npid);
        save_users(db);

        // Remove from cache
        {
            std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
            token_cache.erase(user["token"]);
        }

        // Delete user files
        fs::remove_all("v3kn/" + npid);

        const auto msg = "Deleting account for NPID " + npid;
        log(msg);

        res.set_content("OK:UserDeleted", "text/plain");
    });

    // LOGIN
    v3kn.Post("/v3kn/login", [&](const httplib::Request &req, httplib::Response &res) {
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

        // Check if user exists
        if (!db["users"].contains(npid)) {
            log("Login attempt for non-existing NPID " + npid);
            res.set_content("ERR:UserNotFound", "text/plain");
            return;
        }

        auto &user = db["users"][npid];

        // Get the salt
        const std::string base64_salt = user["salt"];
        const std::string salt_str = base64_decode(base64_salt);
        const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

        // Compute the server hash
        const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
        const std::string base64_server_hash = base64_encode(std::string(server_hash.begin(), server_hash.end()));

        // Compare hashes
        if (user["password"] != base64_server_hash) {
            log("Invalid password on login attempt for NPID " + npid);
            res.set_content("ERR:InvalidPassword", "text/plain");
            return;
        }

        // Get the token
        const std::string token = user["token"];

        // Get quota used
        const uint64_t used = user["quota_used"];

        user["last_login"] = std::time(0);
        user["last_activity"] = std::time(0);
        update_remote_addr(req, user);
        save_users(db);

        // Update cache
        {
            std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
            token_cache[token] = npid;
        }

        const auto msg = "User " + npid + " logged in.";
        log(msg);

        // Respond with token and quota
        res.set_content("OK:" + token + ":" + std::to_string(static_cast<uint64_t>(user["created_at"])) + ":" + std::to_string(used) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
    });

    // Change Username (NPID)
    v3kn.Post("/v3kn/change_npid", [&](const httplib::Request &req, httplib::Response &res) {
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

        // Rename user
        db["users"][new_npid] = db["users"][npid];
        db["users"].erase(npid);

        // Update last activity
        auto &user = db["users"][new_npid];
        user["last_activity"] = std::time(0);
        update_remote_addr(req, user);

        // Update token mapping
        const std::string token = user["token"];
        db["tokens"][token] = new_npid;

        save_users(db);

        // Update cache
        {
            std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
            token_cache[token] = new_npid;
        }

        // Rename user directory
        fs::rename("v3kn/" + npid, "v3kn/" + new_npid);

        const auto msg = "User " + npid + " changed NPID to " + new_npid;
        log(msg);

        res.set_content("OK:NPIDChanged", "text/plain");
    });

    // Change Password
    v3kn.Post("/v3kn/change_password", [&](const httplib::Request &req, httplib::Response &res) {
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

        // Get the salt
        const std::string base64_salt = user["salt"];
        const std::string salt_str = base64_decode(base64_salt);
        const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());

        // Compute the server hash for the old password
        const std::string old_password = base64_decode(base64_old_password);
        const std::vector<unsigned char> old_server_hash = compute_server_hash(old_password, salt);
        const std::string base64_old_server_hash = base64_encode(std::string(old_server_hash.begin(), old_server_hash.end()));

        // Compare hashes
        if (user["password"] != base64_old_server_hash) {
            log("Invalid old password on password change attempt for NPID " + npid);
            res.set_content("ERR:InvalidPassword", "text/plain");
            return;
        }

        // Generate new salt
        const std::vector<unsigned char> new_salt = generate_salt();

        // Compute new server hash
        const std::vector<unsigned char> new_server_hash = compute_server_hash(new_password, new_salt);

        // Delete old token from database and cache
        const std::string old_token = user["token"];
        db["tokens"].erase(old_token);
        {
            std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
            token_cache.erase(old_token);
        }

        // Generate new token
        const std::string new_token = generate_token();

        // Update user with new password, salt, and token
        user["password"] = base64_encode(std::string(new_server_hash.begin(), new_server_hash.end()));
        user["salt"] = base64_encode(std::string(new_salt.begin(), new_salt.end()));
        user["token"] = new_token;
        user["last_activity"] = std::time(0);
        update_remote_addr(req, user);

        // Add new token to database
        db["tokens"][new_token] = npid;

        save_users(db);

        // Add new token to cache
        {
            std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
            token_cache[new_token] = npid;
        }

        const auto msg = "User " + npid + " changed their password (new token generated).";
        log(msg);

        res.set_content("OK:" + new_token, "text/plain");
    });

    //  GET SAVE INFO
    v3kn.Get("/v3kn/save_info", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> req_lock(request_mutex);

        std::string err;
        const std::string npid = get_valid_npid(req, "save info request", err);
        if (npid.empty()) {
            res.set_content(err, "text/plain");
            return;
        }

        // Get TitleID
        auto titleid = req.get_param_value("titleid");
        if (titleid.empty()) {
            log("Missing TitleID on save info request for NPID " + npid);
            res.set_content("ERR:MissingTitleID", "text/plain");
            return;
        }

        const auto msg = "NPID: " + npid + ", TitleID: " + titleid;
        log(msg);

        const std::string savedata_path = "v3kn/" + npid + "/savedata/" + titleid;
        if (!fs::exists(savedata_path)) {
            log("No savedata for NPID " + npid + " TitleID " + titleid);
            res.set_content("WARN:NoSavedata", "text/plain");
            return;
        }

        std::ifstream savedata_info_file(fs::path(savedata_path) / "savedata.xml");
        if (!savedata_info_file) {
            log("No savedata info file for NPID " + npid + " TitleID " + titleid);
            res.set_content("WARN:NoSavedataInfo", "text/plain");
            return;
        }

        std::string savedata_content((std::istreambuf_iterator<char>(savedata_info_file)),
            std::istreambuf_iterator<char>());

        update_last_activity(req, npid);

        res.set_content("OK:" + savedata_content, "application/xml");
    });

    // GET TROPHIES INFO
    v3kn.Get("/v3kn/trophies_info", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> req_lock(request_mutex);

        std::string err;
        const std::string npid = get_valid_npid(req, "trophies info request", err);
        if (npid.empty()) {
            res.set_content(err, "text/plain");
            return;
        }

        const auto msg = "NPID: " + npid + " requesting trophies info";
        log(msg);

        std::ifstream trophies_info_file(fs::path("v3kn") / npid / "trophy" / "trophies.xml");
        if (!trophies_info_file) {
            log("No trophies info file for NPID " + npid);
            res.set_content("WARN:NoTrophiesInfo", "text/plain");
            return;
        }
        std::string trophies_content((std::istreambuf_iterator<char>(trophies_info_file)),
            std::istreambuf_iterator<char>());

        update_last_activity(req, npid);

        res.set_content("OK:" + trophies_content, "application/xml");
    });

    //  DOWNLOAD FILE
    v3kn.Get("/v3kn/download_file", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> req_lock(request_mutex);

        std::string err;
        const std::string npid = get_valid_npid(req, "file download", err);
        if (npid.empty()) {
            res.set_content(err, "text/plain");
            return;
        }

        const auto type = req.get_param_value("type");
        if ((type != "savedata") && (type != "trophy")) {
            log("NPID " + npid + " try to download with invalid type: " + type);
            res.set_content("ERR:InvalidType", "text/plain");
            return;
        }

        const auto id = req.get_param_value("id");
        bool invalid_id = false;
        if (type == "savedata")
            invalid_id = !id.starts_with("PCS") || (id.size() != 9);
        else if (type == "trophy")
            invalid_id = !id.starts_with("NPWR") || (id.size() != 12);

        if (invalid_id) {
            log("NPID " + npid + " try to download with invalid id: " + id);
            res.set_content("ERR:InvalidID", "text/plain");
            return;
        }

        auto msg = "NPID: " + npid + " type: " + type + " id: " + id;

        const auto path = (type == "savedata") ? "savedata.psvimg" : "TROPUSR.DAT";
        const fs::path file_path{ fs::path("v3kn") / npid / type / id / path };

        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            log(msg + ", File not found: " + file_path.string());
            res.set_content("ERR:FileNotFound", "text/plain");
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        msg += "\nServing file: " + file_path.string() + " (" + std::to_string(buffer.str().size()) + " bytes)";
        log(msg);

        update_last_activity(req, npid);

        res.set_content(buffer.str(), "application/octet-stream");
    });

    //  UPLOAD FILE (multipart/form-data)
    v3kn.Post("/v3kn/upload_file", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> req_lock(request_mutex);

        std::string err;
        const std::string npid = get_valid_npid(req, "file upload", err);
        if (npid.empty()) {
            res.set_content(err, "text/plain");
            return;
        }

        const auto type = req.get_param_value("type");
        if ((type != "savedata") && (type != "trophy")) {
            log("NPID " + npid + " try to upload with invalid type: " + type);
            res.set_content("ERR:InvalidType", "text/plain");
            return;
        }

        const auto id = req.get_param_value("id");
        bool invalid_id = false;
        if (type == "savedata")
            invalid_id = !id.starts_with("PCS") || (id.size() != 9);
        else if (type == "trophy")
            invalid_id = !id.starts_with("NPWR") || (id.size() != 12);

        if (invalid_id) {
            log("NPID " + npid + " try to upload with invalid id: " + id);
            res.set_content("ERR:InvalidID", "text/plain");
            return;
        }

        auto msg = "NPID: " + npid + " type: " + type + " id: " + id;

        // Get file from multipart
        if (!req.form.has_file("file")) {
            log(msg + ", missing file on upload attempt");
            res.set_content("ERR:MissingFile", "text/plain");
            return;
        }

        const auto file = req.form.get_file("file");
        const uint64_t newSize = file.content.size();

        const fs::path base_path{ fs::path("v3kn") / npid / type / id };
        const std::string path = (type == "savedata") ? "savedata.psvimg" : "TROPUSR.DAT";
        const fs::path file_path{ base_path / path };

        uint64_t oldSize = 0;
        if (fs::exists(file_path))
            oldSize = fs::file_size(file_path);

        const int64_t delta = (int64_t)newSize - (int64_t)oldSize;

        uint64_t new_used = 0;
        {
            std::lock_guard<std::mutex> lock_db(account_mutex);
            json db = load_users();
            auto &user = db["users"][npid];

            uint64_t used = user["quota_used"];
            new_used = used + delta;

            if ((delta > 0) && (new_used > DEFAULT_QUOTA_TOTAL)) {
                log(msg + ", exceeded quota on upload attempt. Used: " + std::to_string(used) + ", New Used: " + std::to_string(new_used) + ", Total: " + std::to_string(DEFAULT_QUOTA_TOTAL));
                res.set_content("ERR:QuotaExceeded", "text/plain");
                return;
            }

            user["quota_used"] = new_used;
            user["last_activity"] = std::time(0);
            save_users(db);
        }

        fs::create_directories(base_path);
        {
            std::ofstream out(file_path, std::ios::binary);
            out << file.content;
        }

        // Get XML from multipart (sent as text field, not file)
        if (req.form.has_field("xml")) {
            const auto xml_content = req.form.get_field("xml");
            const fs::path xml_path{ (type == "savedata") ? base_path / "savedata.xml" : base_path.parent_path() / "trophies.xml" };
            std::ofstream xml_out(xml_path, std::ios::binary);
            xml_out << xml_content;
        }

        log(msg + "\nUploaded file " + file_path.string() + " (" + std::to_string(newSize) + " bytes), quota: " + std::to_string(new_used) + " / " + std::to_string(DEFAULT_QUOTA_TOTAL));
        res.set_content("OK:" + std::to_string(new_used) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
    });

#ifndef _WIN32 // Disable auto-updater on Windows
    // AUTO-UPDATER THREAD
    std::thread([]() {
        while (true) {
            // Sleep for 5 minutes
            std::this_thread::sleep_for(std::chrono::minutes(5));
            std::string server_hash = app_hash;
            httplib::Client cli("https://api.github.com");
            auto res = cli.Get("/repos/Zangetsu38/v3kn/releases/tags/continuous");
            if (res && res->status == 200) {
                const auto &body = res->body;
                std::string pattern = "Corresponding commit:\\s*([a-f0-9]{" + std::to_string(server_hash.size()) + "})";
                std::regex re(pattern);
                std::smatch match;
                if (std::regex_search(body, match, re)) {
                    if (match.size() == 2) {
                        std::string latest_hash = match[1].str();
                        if (latest_hash != server_hash) {
                            const auto msg = "Update available, Current: " + server_hash + ", Latest: " + latest_hash;
                            log(msg);

                            // Wait for all requests to finish before updating
                            std::lock_guard<std::mutex> update_lock(request_mutex);
                            log("All requests finished, starting update...");
                            system("nohup ./update-v3kn.sh &");
                        }
                    }
                }
            } else
                std::cout << "Failed to check for updates. HTTP Status: " << (res ? std::to_string(res->status) : "No Response") << std::endl;
        }
    }).detach();
#endif

    // clean old log file
    std::ofstream("v3kn.log", std::ios::trunc);

    // Pre-load token cache from users.json to avoid file I/O during uploads
    {
        json db = load_users();
        if (db.contains("tokens") && db["tokens"].is_object()) {
            std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
            for (auto &[token, npid] : db["tokens"].items()) {
                token_cache[token] = npid.get<std::string>();
            }
            std::cout << "[INFO] Loaded " << token_cache.size() << " tokens into cache" << std::endl;
        }
    }

    //  START SERVER
    std::cout << "v3kn server running on port 3000..." << std::endl;
    v3kn.listen("0.0.0.0", 3000);

    return 0;
}
