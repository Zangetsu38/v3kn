// v3knr project
// Copyright (C) 2026 Vita3K team

#include "utils/utils.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>

// Global mutexes
std::mutex account_mutex;
std::mutex request_mutex;
std::mutex stitles_cache_mutex;
std::mutex log_mutex;

// Cache Mutexes
std::mutex account_id_cache_mutex;
std::mutex online_id_cache_mutex;
std::mutex token_cache_mutex;

// Global caches
std::unordered_map<std::string, std::string> account_id_cache; // account_id -> online_id
std::unordered_map<std::string, std::string> online_id_cache; // online_id -> account_id
std::unordered_map<std::string, std::string> token_cache; // token -> account_id

// Condition variable for messages long polling
std::mutex messages_cv_mutex;
std::condition_variable messages_cv;

// Condition variable for friends long polling
std::mutex friends_cv_mutex;
std::condition_variable friends_cv;

static json stitles_cache = json{ { "titles", json::object() } };

// Database operations
std::mutex profile_mutex;
json load_profile(const std::string &online_id) {
    std::lock_guard<std::mutex> lock(profile_mutex);
    const fs::path profile_path{ fs::path("v3kn") / "Users" / online_id / "profile.json" };
    if (fs::exists(profile_path)) {
        std::ifstream profile_file(profile_path);
        try {
            json profile;
            profile_file >> profile;
            return profile;
        } catch (...) {
            // If the file is corrupted -> recreate a clean profile
            json profile = json::object();
            profile["online_id"] = online_id;
            log("Corrupted profile for online ID " + online_id + " - recreating");
            return profile;
        }
    } else {
        // Create a minimal profile
        json profile = json::object();
        profile["online_id"] = online_id;
        log("Creating new profile for online ID " + online_id);
        return profile;
    }
}

void save_profile(const std::string &online_id, const json &profile) {
    std::lock_guard<std::mutex> lock(profile_mutex);
    const fs::path profile_path{ fs::path("v3kn") / "Users" / online_id / "profile.json" };
    std::ofstream profile_file_out(profile_path);
    profile_file_out << profile.dump(2);
}

json load_users() {
    std::ifstream f("v3kn/users.json");
    if (!f.is_open())
        return json{ { "users", json::object() } };
    json db;
    f >> db;
    return db;
}

void save_users(const json &db) {
    std::ofstream f("v3kn/users.json");
    f << db.dump(2);
}

json load_stitles() {
    std::ifstream f("v3kn/stitles.json");
    if (!f.is_open())
        return json{ { "stitles", json::object() } };

    json db;
    f >> db;
    if (!db.contains("stitles") || !db["stitles"].is_object())
        db["stitles"] = json::object();
    return db;
}

void save_stitles(const json &db) {
    std::ofstream f("v3kn/stitles.json");
    f << db.dump(2);
}

void reload_stitles_cache() {
    std::lock_guard<std::mutex> lock(stitles_cache_mutex);
    stitles_cache = load_stitles();
}

bool has_stitle_info(const std::string &titleid) {
    std::lock_guard<std::mutex> lock(stitles_cache_mutex);
    return stitles_cache.contains("stitles") && stitles_cache["stitles"].contains(titleid);
}

void update_stitle_info(const std::string &titleid, const json &names) {
    std::lock_guard<std::mutex> lock(stitles_cache_mutex);
    if (!stitles_cache.contains("stitles") || !stitles_cache["stitles"].is_object())
        stitles_cache["stitles"] = json::object();

    stitles_cache["stitles"][titleid]["names"] = names;
    stitles_cache["stitles"][titleid]["updated_at"] = std::time(0);
    save_stitles(stitles_cache);
}

std::string get_stitle_name(const std::string &titleid, const std::string &language) {
    std::lock_guard<std::mutex> lock(stitles_cache_mutex);
    if (!stitles_cache.contains("stitles") || !stitles_cache["stitles"].contains(titleid))
        return titleid;

    const auto &entry = stitles_cache["stitles"][titleid];
    if (!entry.contains("names") || !entry["names"].is_object())
        return titleid;

    const auto &names = entry["names"];
    if (!language.empty() && names.contains(language) && names[language].is_string())
        return names[language].get<std::string>();
    if (names.contains("default") && names["default"].is_string())
        return names["default"].get<std::string>();

    return titleid;
}

size_t get_stitles_cache_size() {
    std::lock_guard<std::mutex> lock(stitles_cache_mutex);
    if (!stitles_cache.contains("stitles") || !stitles_cache["stitles"].is_object())
        return 0;
    return stitles_cache["stitles"].size();
}

// Token operations
std::string generate_token() {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string token;
    token.resize(48);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);

    for (size_t i = 0; i < 48; ++i)
        token[i] = chars[dist(gen)];

    return token;
}

std::string get_token_from_request(const httplib::Request &req) {
    if (!req.has_header("Authorization"))
        return "";

    std::string h = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";

    if (h.rfind(prefix, 0) == 0)
        return h.substr(prefix.size());

    return "";
}

std::string get_account_id_from_token(const std::string &token) {
    std::lock_guard<std::mutex> lock(token_cache_mutex);
    auto it = token_cache.find(token);
    if (it != token_cache.end()) {
        return it->second;
    }
    return "";
}

std::string get_account_id_from_online_id(const std::string &online_id) {
    std::lock_guard<std::mutex> lock(online_id_cache_mutex);
    auto it = online_id_cache.find(online_id);
    if (it != online_id_cache.end()) {
        return it->second;
    }
    return "";
}

static std::string get_valid_account_id(const httplib::Request &req, const std::string &request, std::string &err) {
    const std::string token = get_token_from_request(req);
    if (token.empty()) {
        log("Missing token on request: " + request);
        err = "ERR:MissingToken";
        return {};
    }

    const std::string account_id = get_account_id_from_token(token);
    if (account_id.empty()) {
        log("Invalid token on request: " + request);
        err = "ERR:InvalidToken";
        return {};
    }

    return account_id;
}

std::optional<UserAccount> get_valid_account(const httplib::Request &req, const std::string &request, std::string &err) {
    const std::string account_id = get_valid_account_id(req, request, err);
    if (account_id.empty())
        return std::nullopt;

    const std::string online_id = get_online_id_from_account_id(account_id);
    if (online_id.empty()) {
        err = "ERR:OnlineIDNotFound";
        return std::nullopt;
    }

    return UserAccount{ account_id, online_id };
}

std::optional<UserAccount> get_valid_target_account(const httplib::Request &req, const std::string &request, std::string &err, const std::string &online_id) {
    // Get target online_id from parameters
    std::string target_online_id = req.get_param_value("target_online_id");
    if (target_online_id.empty()) {
        log("Online ID " + online_id + " try to get activities with missing target online ID");
        err = "ERR:MissingTargetOnlineID";
        return std::nullopt;
    }

    const std::string target_account_id = get_account_id_from_online_id(target_online_id);
    if (target_account_id.empty()) {
        log("Online ID " + online_id + " try to get activities for non-existing Account with online ID " + target_online_id);
        err = "ERR:TargetAccountNotFound";
        return std::nullopt;
    }

    target_online_id = get_online_id_from_account_id(target_account_id);
    if (target_online_id.empty()) {
        log("Online ID " + online_id + " try to get activities for non-existing online ID " + target_online_id);
        err = "ERR:TargetOnlineIDNotFound";
        return std::nullopt;
    }

    return UserAccount{ target_account_id, target_online_id };
}

std::string get_online_id_from_account_id(const std::string &account_id) {
    std::lock_guard<std::mutex> lock(account_id_cache_mutex);
    auto it = account_id_cache.find(account_id);
    if (it != account_id_cache.end()) {
        return it->second;
    }

    return "";
}

// Crypto operations
std::vector<unsigned char> generate_salt() {
    const size_t length = 64;
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

std::vector<unsigned char> compute_server_hash(const std::string &client_hash, const std::vector<unsigned char> &salt) {
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

// String operations
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64_decode(const std::string &encoded) {
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

std::string base64_encode(const std::string &input) {
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

std::string lowercase_online_id(std::string online_id) {
    std::transform(online_id.begin(), online_id.end(), online_id.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return online_id;
}

std::string trim_online_id(std::string online_id) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };

    online_id.erase(online_id.begin(),
        std::find_if(online_id.begin(), online_id.end(),
            [&](unsigned char c) { return !is_space(c); }));

    online_id.erase(std::find_if(online_id.rbegin(), online_id.rend(),
                        [&](unsigned char c) { return !is_space(c); })
                        .base(),
        online_id.end());

    return online_id;
}

// Network operations
std::string get_remote_addr(const httplib::Request &req) {
    const std::string ip = req.get_header_value("CF-Connecting-IP");
    return ip.empty() ? req.remote_addr : ip;
}

void update_remote_addr(const httplib::Request &req, json &user) {
    const auto remote_addr = get_remote_addr(req);
    if (!user.contains("remote_addr") || !user["remote_addr"].is_array())
        user["remote_addr"] = json::array();

    if (std::none_of(user["remote_addr"].begin(), user["remote_addr"].end(),
            [&](const json &v) { return v.get<std::string>() == remote_addr; })) {
        user["remote_addr"].push_back(remote_addr);
    }
}

void update_last_activity(const httplib::Request &req, const std::string &account_id) {
    std::lock_guard<std::mutex> lock(account_mutex);
    json db = load_users();
    if (!db.contains("users") || !db["users"].contains(account_id))
        return;

    auto &user = db["users"][account_id];
    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);
    save_users(db);
}

// Logging
void log(const std::string &msg) {
    std::lock_guard<std::mutex> lock(log_mutex);

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

    std::string full = std::string("[") + timebuf + "] " + msg;

    std::cout << full << std::endl;

    {
        std::ofstream root("v3kn.log", std::ios::app);
        root << full << "\n";
    }

    char yearbuf[8], monthbuf[8], daybuf[8];
    std::strftime(yearbuf, sizeof(yearbuf), "%Y", &tm);
    std::strftime(monthbuf, sizeof(monthbuf), "%m", &tm);
    std::strftime(daybuf, sizeof(daybuf), "%d", &tm);

    fs::path folder = fs::path("logs") / yearbuf / monthbuf;
    fs::create_directories(folder);

    fs::path filepath = folder / (std::string(daybuf) + ".log");

    std::ofstream f(filepath, std::ios::app);
    f << full << "\n";
}
