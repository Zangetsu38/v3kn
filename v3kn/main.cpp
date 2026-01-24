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

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>

using json = nlohmann::json;

namespace fs = std::filesystem;

std::mutex account_mutex;

// ------------------------------------------------------------
//  UTILITIES
// ------------------------------------------------------------

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

static std::string get_metadata_from_request(const httplib::Request &req) {
    if (!req.has_header("Metadata"))
        return "";

    return req.get_header_value("Metadata");
}

static std::string get_npid_from_token(const std::string &token) {
    json db = load_users();
    if (!db.contains("users") || !db.contains("tokens"))
        return "";

    if (!db["tokens"].contains(token))
        return "";

    return db["tokens"][token];
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

static void log(const std::string &msg) {
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

    // 2. Complet message.
    std::string full = std::string("[") + timebuf + "] " + msg;

    // 3. Console
    std::cout << full << std::endl;

    // 4. Log File
    std::ofstream f("v3kn.log", std::ios::app);
    f << full << "\n";
}

static constexpr uint64_t DEFAULT_QUOTA_TOTAL = 50 * 1024 * 1024; // 50 MB
std::mutex request_mutex;

//  SERVER
int main() {
    auto seed = static_cast<unsigned int>(
        std::chrono::system_clock::now().time_since_epoch().count());
    srand(seed);
    httplib::Server v3kn;

    v3kn.set_logger([](const httplib::Request &req, const httplib::Response &) {
        const std::string cn = req.get_header_value("CF-IPCountry");
        const std::string ip = req.get_header_value("CF-Connecting-IP");
        const std::string remote_addr = ip.empty() ? req.remote_addr : ip;
        std::string msg = req.method + " " + req.path + " from [" + cn + "] " + remote_addr + ":" + std::to_string(req.remote_port);

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

    // GET User Info
    v3kn.Get("/info", [&](const httplib::Request &req, httplib::Response &res) {
        std::string html = R"(
        <html>
            <head><title>v3kn Info</title></head>
            <body>
                <h1>v3kn User Info</h1>
                <form method="get" action="/info">
                    NPID: <input type="text" name="npid"><br>
                    Password (base64): <input type="text" name="password"><br>
                    <input type="submit" value="Get Info">
                </form>
        )";
        const std::string npid = req.get_param_value("npid");
        const std::string base64_password = req.get_param_value("password");
        if (npid.empty() || base64_password.empty()) {
            html += "<p>Please enter your NPID and password to see your info.</p>";
        } else {
            const std::string base64_password = req.get_param_value("password");
            const std::string password = base64_decode(base64_password);
            json db = load_users();
            if (db["users"].contains(npid)) {
                auto &user = db["users"][npid];
                // Get the salt
                const std::string base64_salt = user["salt"];
                const std::string salt_str = base64_decode(base64_salt);
                const std::vector<unsigned char> salt(salt_str.begin(), salt_str.end());
                // Compute the server hash
                const std::vector<unsigned char> server_hash = compute_server_hash(password, salt);
                const std::string base64_server_hash = base64_encode(std::string(server_hash.begin(), server_hash.end()));
                if (user["password"] == base64_server_hash) {
                    html += "<p>Account created at: " + std::to_string(static_cast<uint64_t>(user["created_at"])) + "</p>";
                    html += "<p>Last login: " + std::to_string(static_cast<uint64_t>(user["last_login"])) + "</p>";
                    html += "<p>Last activity: " + std::to_string(static_cast<uint64_t>(user["last_activity"])) + "</p>";
                    html += "<p>Quota used: " + std::to_string(static_cast<uint64_t>(user["quota_used"])) + " / " + std::to_string(DEFAULT_QUOTA_TOTAL) + "</p>";
                } else {
                    html += "<p>Invalid password.</p>";
                }
            }
        }
        html += R"(
            </body>
        </html>
        )";

        res.set_content(html, "text/html");
    });

    // GET QUOTA
    v3kn.Get("/v3kn/quota", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);
        const std::string token = get_token_from_request(req);
        if (token.empty()) {
            log("Missing token on quota request");
            res.set_content("ERR:MissingToken", "text/plain");
            return;
        }

        const std::string npid = get_npid_from_token(token);
        if (npid.empty()) {
            log("Invalid token on quota request");
            res.set_content("ERR:InvalidToken", "text/plain");
            return;
        }

        update_last_activity(req, npid);

        json db = load_users();
        auto &user = db["users"][npid];
        const uint64_t used = user["quota_used"];
        const uint64_t total = DEFAULT_QUOTA_TOTAL;

        const auto msg = "Quota for NPID " + npid + ": " + std::to_string(used) + " / " + std::to_string(total);
        log(msg);

        res.set_content("OK:" + std::to_string(used) + ":" + std::to_string(total), "text/plain");
    });

    //  CREATE ACCOUNT
    v3kn.Post("/v3kn/create", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);

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

        std::filesystem::create_directories("v3kn/" + npid + "/savedata");

        const auto msg = "Created account for NPID " + npid;
        log(msg);

        res.set_content("OK:" + token + ":" + std::to_string(std::time(0)) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
    });

    // Delete
    v3kn.Post("/v3kn/delete", [&](const httplib::Request &req, httplib::Response &res) {
        const std::string token = get_token_from_request(req);
        if (token.empty()) {
            log("Missing token on account deletion attempt");
            res.set_content("ERR:MissingToken", "text/plain");
            return;
        }

        const std::string npid = get_npid_from_token(token);
        if (npid.empty()) {
            log("Invalid token on account deletion attempt");
            res.set_content("ERR:InvalidToken", "text/plain");
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

        // Delete user files
        fs::remove_all("v3kn/" + npid);

        const auto msg = "Deleting account for NPID " + npid;
        log(msg);

        res.set_content("OK:UserDeleted", "text/plain");
    });

    // LOGIN
    v3kn.Post("/v3kn/login", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);
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

        const auto msg = "User " + npid + " logged in.";
        log(msg);

        // Respond with token and quota
        res.set_content("OK:" + token + ":" + std::to_string(static_cast<uint64_t>(user["created_at"])) + ":" + std::to_string(used) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
    });

    // Change Username (NPID)
    v3kn.Post("/v3kn/change_npid", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);
        const std::string token = get_token_from_request(req);
        if (token.empty()) {
            log("Missing token on NPID change attempt");
            res.set_content("ERR:MissingToken", "text/plain");
            return;
        }

        const std::string npid = get_npid_from_token(token);
        if (npid.empty()) {
            log("Invalid token on NPID change attempt");
            res.set_content("ERR:InvalidToken", "text/plain");
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
        auto &user = db["users"][new_npid];
        user["last_activity"] = std::time(0);
        update_remote_addr(req, user);

        db["users"].erase(npid);

        // Update token mapping
        db["tokens"][token] = new_npid;

        save_users(db);

        // Rename user directory
        fs::rename("v3kn/" + npid, "v3kn/" + new_npid);

        const auto msg = "User " + npid + " changed NPID to " + new_npid;
        log(msg);

        res.set_content("OK:NPIDChanged", "text/plain");
    });

    // Change Password
    v3kn.Post("/v3kn/change_password", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);
        const std::string token = get_token_from_request(req);
        if (token.empty()) {
            log("Missing token on password change attempt");
            res.set_content("ERR:MissingToken", "text/plain");
            return;
        }

        const std::string npid = get_npid_from_token(token);
        if (npid.empty()) {
            log("Invalid token on password change attempt");
            res.set_content("ERR:InvalidToken", "text/plain");
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

        // Update user password and salt
        user["password"] = base64_encode(std::string(new_server_hash.begin(), new_server_hash.end()));
        user["salt"] = base64_encode(std::string(new_salt.begin(), new_salt.end()));
        user["last_activity"] = std::time(0);
        update_remote_addr(req, user);

        save_users(db);

        const auto msg = "User " + npid + " changed their password.";
        log(msg);

        res.set_content("OK:PasswordChanged", "text/plain");
    });

    //  GET SAVE INFO
    v3kn.Get("/v3kn/save_info", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);
        const std::string token = get_token_from_request(req);
        if (token.empty()) {
            log("Missing token on save info request");
            res.set_content("ERR:MissingToken", "text/plain");
            return;
        }

        // Get NPID from token
        const std::string npid = get_npid_from_token(token);
        if (npid.empty()) {
            log("Invalid token on save info request");
            res.set_content("ERR:InvalidToken", "text/plain");
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

        update_last_activity(req, npid);

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

        res.set_content("OK:" + savedata_content, "application/xml");
    });

    //  DOWNLOAD FILE
    v3kn.Get("/v3kn/download_file", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);
        const std::string token = get_token_from_request(req);
        const std::string npid = get_npid_from_token(token);

        if (npid.empty()) {
            log("Invalid token on file download attempt");
            res.set_content("ERR:InvalidToken", "text/plain");
            return;
        }

        update_last_activity(req, npid);

        const auto type = req.get_param_value("type");
        if (type != "savedata") {
            log("NPID " + npid + " try to download with invalid type: " + type);
            res.set_content("ERR:InvalidType", "text/plain");
            return;
        }

        const auto id = req.get_param_value("id");
        if (!id.starts_with("PCS")) {
            log("NPID " + npid + " try to download with invalid id: " + id);
            res.set_content("ERR:InvalidID", "text/plain");
            return;
        }

        auto msg = "NPID: " + npid + " type: " + type + " id: " + id;

        std::string file_path = "v3kn/" + npid + "/" + type + "/" + id + "/" + "savedata.psvimg";

        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            log(msg + ", File not found: " + file_path);
            res.set_content("ERR:FileNotFound", "text/plain");
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        msg += "\nServing file: " + file_path + " (" + std::to_string(buffer.str().size()) + " bytes)";
        log(msg);

        res.set_content(buffer.str(), "application/octet-stream");
    });

    //  UPLOAD FILE
    v3kn.Put("/v3kn/upload_file", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(request_mutex);

        // Auth
        std::string token = get_token_from_request(req);

        // Metadata
        std::string metadata = get_metadata_from_request(req);

        // NPID
        std::string npid = get_npid_from_token(token);

        if (npid.empty()) {
            log("Invalid token on file upload attempt");
            res.set_content("ERR:InvalidToken", "text/plain");
            return;
        }

        update_last_activity(req, npid);

        const auto type = req.get_param_value("type");
        if (type != "savedata") {
            log("NPID " + npid + " try to upload with invalid type: " + type);
            res.set_content("ERR:InvalidType", "text/plain");
            return;
        }

        const auto id = req.get_param_value("id");
        if (!id.starts_with("PCS") || (id.size() != 9)) {
            log("NPID " + npid + " try to upload with invalid id: " + id);
            res.set_content("ERR:InvalidID", "text/plain");
            return;
        }

        const std::string base_path = "v3kn/" + npid + "/" + type + "/" + id + "/" + "savedata.psvimg";

        // Load the DB
        std::lock_guard<std::mutex> lock_db(account_mutex);
        json db = load_users();
        auto &user = db["users"][npid];

        // Quota info
        uint64_t used = user["quota_used"];

        // Old size
        uint64_t oldSize = 0;
        if (fs::exists(base_path))
            oldSize = fs::file_size(base_path);

        // New size
        const uint64_t newSize = req.body.size();

        // Real delta
        const int64_t delta = (int64_t)newSize - (int64_t)oldSize;
        const uint64_t new_used = used + delta;

        // Quota check
        if ((delta > 0) && (new_used > DEFAULT_QUOTA_TOTAL)) {
            const auto msg = "Quota exceeded for NPID: " + npid + " " + std::to_string(new_used) + " / " + std::to_string(DEFAULT_QUOTA_TOTAL);
            log(msg);
            res.set_content("ERR:QuotaExceeded", "text/plain");
            return;
        }

        // OK -> write the file
        fs::create_directories(fs::path(base_path).parent_path());
        {
            std::ofstream out(base_path, std::ios::binary);
            out << req.body;
        }

        // Write the XML
        {
            // Decode and write metadata
            fs::path xml_path = fs::path(base_path).parent_path() / "savedata.xml";
            const auto metadata_xml = base64_decode(metadata);
            std::ofstream xml_out(xml_path, std::ios::binary);
            xml_out << metadata_xml;
        }

        // Update quota
        user["quota_used"] = new_used;

        auto msg = "Quota used updated for NPID " + npid + ": " + std::to_string(new_used) + " / " + std::to_string(DEFAULT_QUOTA_TOTAL);
        msg += "\nUploaded file for id: " + id + " (" + std::to_string(newSize) + " bytes)";
        log(msg);

        save_users(db);

        res.set_content("OK:" + std::to_string(new_used) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
    });

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
                            std::lock_guard<std::mutex> lock(request_mutex);
                            system("nohup ./update-v3kn.sh &");
                        }
                    }
                }
            } else
                std::cout << "Failed to check for updates. HTTP Status: " << (res ? std::to_string(res->status) : "No Response") << std::endl;
        }
    }).detach();

    //  START SERVER
    std::cout << "v3kn server running on port 3000...\nversion: " << app_hash << std::endl;
    v3kn.listen("0.0.0.0", 3000);

    return 0;
}
