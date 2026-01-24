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
std::mutex token_cache_mutex;
std::mutex log_mutex;

// Condition variable for messages long polling
std::mutex messages_cv_mutex;
std::condition_variable messages_cv;

// Condition variable for friends long polling
std::mutex friends_cv_mutex;
std::condition_variable friends_cv;

// Token cache
std::unordered_map<std::string, std::string> token_cache;

// Database operations
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
    f << db.dump(4);
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

std::string get_npid_from_token(const std::string &token) {
    std::lock_guard<std::mutex> lock(token_cache_mutex);
    auto it = token_cache.find(token);
    if (it != token_cache.end()) {
        return it->second;
    }
    return "";
}

std::string get_valid_npid(const httplib::Request &req, const std::string &request, std::string &err) {
    const std::string token = get_token_from_request(req);
    if (token.empty()) {
        log("Missing token on request: " + request);
        err = "ERR:MissingToken";
        return {};
    }

    const std::string npid = get_npid_from_token(token);
    if (npid.empty()) {
        log("Invalid token on request: " + request);
        err = "ERR:InvalidToken";
        return {};
    }

    return npid;
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

std::string trim_npid(std::string npid) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };

    npid.erase(npid.begin(),
        std::find_if(npid.begin(), npid.end(),
            [&](unsigned char c) { return !is_space(c); }));

    npid.erase(std::find_if(npid.rbegin(), npid.rend(),
                   [&](unsigned char c) { return !is_space(c); })
                   .base(),
        npid.end());

    return npid;
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

void update_last_activity(const httplib::Request &req, const std::string &npid) {
    std::lock_guard<std::mutex> lock(account_mutex);
    json db = load_users();
    if (!db.contains("users") || !db["users"].contains(npid))
        return;

    auto &user = db["users"][npid];
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
