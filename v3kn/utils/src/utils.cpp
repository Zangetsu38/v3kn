// v3knr project
// Copyright (C) 2026 Vita3K team

#include "utils/utils.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Global mutexes
std::mutex users_mutex;
std::mutex account_mutex;
std::mutex stitles_cache_mutex;
std::mutex log_mutex;

// Cache Mutexes
std::mutex online_id_cache_mutex;
std::mutex token_cache_mutex;

// Global caches
std::unordered_map<std::string, std::string> online_id_cache; // online_id -> account_id
std::unordered_map<std::string, std::string> token_cache; // token -> account_id

// Condition variable for messages long polling
std::mutex messages_cv_mutex;
std::condition_variable messages_cv;

// Condition variable for friends long polling
std::mutex friends_cv_mutex;
std::condition_variable friends_cv;

static std::unordered_map<std::string, std::vector<json>> poll_events;
static std::mutex poll_events_mutex;
static std::mutex poll_events_file_mutex;

static json stitles_cache = json{ { "titles", json::object() } };

void load_poll_events_from_disk() {
    std::ifstream f("v3kn/events.json");
    if (!f.is_open())
        return;

    json data;
    f >> data;
    if (!data.is_object())
        return;

    std::lock_guard<std::mutex> lock(poll_events_mutex);
    poll_events.clear();
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (it.value().is_array())
            poll_events[it.key()] = it.value().get<std::vector<json>>();
    }
}

static void save_poll_events_to_disk() {
    std::lock_guard<std::mutex> file_lock(poll_events_file_mutex);
    json data = json::object();
    for (const auto &[account_id, events] : poll_events) {
        data[account_id] = events;
    }

    std::ofstream f("v3kn/events.json");
    f << data.dump(2);
}

bool update_poll_events(const std::string &account_id, const std::function<void(json &events)> &updater) {
    std::lock_guard<std::mutex> lock(poll_events_mutex);
    json events = json::array();
    auto it = poll_events.find(account_id);
    if (it != poll_events.end())
        events = it->second;

    const json before = events;
    updater(events);
    if (events == before)
        return false;

    if (events.empty())
        poll_events.erase(account_id);
    else
        poll_events[account_id] = events.get<std::vector<json>>();

    save_poll_events_to_disk();
    return true;
}

json pop_poll_events(const std::string &account_id) {
    std::lock_guard<std::mutex> lock(poll_events_mutex);
    json result = json::array();
    auto it = poll_events.find(account_id);
    if (it != poll_events.end()) {
        result = it->second;
        poll_events.erase(it);
        save_poll_events_to_disk();
    }
    return result;
}

void cleanup_old_poll_events(int64_t max_age_seconds) {
    const int64_t now = std::time(0);
    std::lock_guard<std::mutex> lock(poll_events_mutex);
    bool changed = false;

    for (auto it = poll_events.begin(); it != poll_events.end();) {
        auto &events = it->second;
        events.erase(
            std::remove_if(events.begin(), events.end(),
                [now, max_age_seconds](const json &event) {
                    const std::string type = event.value("type", "");
                    const int64_t event_time = type == "activity"
                        ? event.value("created_at", int64_t{ 0 })
                        : event.value("at", int64_t{ 0 });
                    return event_time != 0 && (now - event_time) > max_age_seconds;
                }),
            events.end());

        if (events.empty()) {
            it = poll_events.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    if (changed)
        save_poll_events_to_disk();
}

// Database operations
static json load_profile_unlocked(const std::string &online_id) {
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

static void save_profile_unlocked(const std::string &online_id, const json &profile) {
    const fs::path profile_path{ fs::path("v3kn") / "Users" / online_id / "profile.json" };
    std::ofstream profile_file_out(profile_path);
    profile_file_out << profile.dump(2);
}

json load_profile(const std::string &online_id) {
    ResourceReadLock lock("profile:" + online_id);
    return load_profile_unlocked(online_id);
}

void save_profile(const std::string &online_id, const json &profile) {
    ResourceLock lock("profile:" + online_id);
    save_profile_unlocked(online_id, profile);
}

void update_profile(const std::string &online_id, const std::function<void(json &)> &updater) {
    ResourceLock lock("profile:" + online_id);
    json profile = load_profile_unlocked(online_id);
    updater(profile);
    save_profile_unlocked(online_id, profile);
}

json users = json::object();
json accounts = json::object();
std::atomic<bool> users_failed = false;

bool is_unsafe_account_path(const fs::path &path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return true;
#endif
    return fs::is_symlink(fs::symlink_status(path));
}

static json read_json_with_backup(const fs::path &path, std::vector<std::pair<fs::path, fs::path>> &files_to_restore) {
    fs::path backup = path;
    backup += ".previous";
    if (is_unsafe_account_path(path) || is_unsafe_account_path(backup))
        throw std::runtime_error("Unsafe account file path: " + path.string());
    json data;
    std::ifstream input(path);
    if (input.is_open()) {
        try {
            input >> data;
            if (!input.bad())
                return data;
        } catch (const json::parse_error &) {
        }
    }
    std::ifstream saved(backup);
    if (!saved.is_open())
        throw std::runtime_error("Cannot read account file or backup: " + path.string());
    saved >> data;
    if (saved.bad())
        throw std::runtime_error("Cannot read account backup: " + backup.string());
    files_to_restore.emplace_back(backup, path);
    return data;
}

static json load_account(const std::string &online_id, std::set<std::string> &names,
    std::vector<std::pair<fs::path, fs::path>> &files_to_restore) {
    if (online_id.empty() || online_id == "." || online_id == ".."
        || online_id.find('\0') != std::string::npos || fs::path(online_id).has_parent_path()
        || fs::path(online_id).has_root_name())
        throw std::runtime_error("Invalid online ID path");
    std::string directory_name = online_id;
#ifdef _WIN32
    std::transform(directory_name.begin(), directory_name.end(), directory_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    if (!names.insert(directory_name).second)
        throw std::runtime_error("duplicate online ID path");
    const fs::path directory = fs::path("v3kn") / "Users" / online_id;
    if (is_unsafe_account_path(directory) || !fs::is_directory(directory))
        throw std::runtime_error("Invalid account directory: " + online_id);
    json user = read_json_with_backup(directory / "account.json", files_to_restore);
    if (!user.is_object() || !user.contains("online_id") || user["online_id"] != online_id)
        throw std::runtime_error("Account does not match index: " + online_id);
    return user;
}

void load_users() {
    const fs::path root = fs::path("v3kn") / "Users";
    const fs::path index_path = fs::path("v3kn") / "users.json";
    fs::path index_backup = index_path;
    index_backup += ".previous";
    if (fs::exists(fs::path("v3kn") / "users.transaction.json"))
        throw std::runtime_error("Pending account transaction: start the previous server version to recover it");
    if (is_unsafe_account_path("v3kn") || is_unsafe_account_path(root))
        throw std::runtime_error("Unsafe account storage path");
    fs::create_directories(root);
    if (is_unsafe_account_path(index_path) || is_unsafe_account_path(index_backup))
        throw std::runtime_error("Unsafe account index path");

    std::vector<std::pair<fs::path, fs::path>> files_to_restore;
    if (!fs::exists(index_path) && !fs::exists(index_backup)) {
        if (!fs::is_empty(root) || fs::exists(fs::path("v3kn") / "users.json.bak"))
            throw std::runtime_error("Account index missing from existing storage");
        std::ofstream output(index_path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << json{ { "users", json::object() } }.dump(2);
        output.close();
    }

    json index = read_json_with_backup(index_path, files_to_restore);
    if (!index.is_object() || !index.contains("users") || !index["users"].is_object()
        || (index.contains("version") ? index.size() != 2 || index["version"] != 2 : index.size() != 1))
        throw std::runtime_error("Invalid account index");

    json loaded_accounts = json::object();
    std::set<std::string> names;
    for (const auto &[account_id, name] : index["users"].items()) {
        if (name.is_object())
            throw std::runtime_error("legacy account database unsupported; migrate with an older version");
        if (account_id.empty() || !name.is_string())
            throw std::runtime_error("Invalid account index entry");
        const std::string online_id = name.get<std::string>();
        loaded_accounts[account_id] = load_account(online_id, names, files_to_restore);
    }

    for (const auto &entry : fs::directory_iterator(root)) {
        if (!entry.is_directory() || !fs::exists(entry.path() / "account.json"))
            continue;
        std::string name = entry.path().filename().string();
#ifdef _WIN32
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        if (!names.contains(name))
            throw std::runtime_error("Account index conflicts with an existing account directory: " + name);
    }
    for (const auto &[backup, path] : files_to_restore) {
        fs::copy_file(backup, path, fs::copy_options::overwrite_existing);
        log("Restored account file from backup: " + path.string());
    }

    users = std::move(index["users"]);
    accounts = std::move(loaded_accounts);
}

void save_account(const std::string &account_id, const json &user) {
    if (users_failed)
        throw std::runtime_error("Account storage unavailable");
    const fs::path root = fs::path("v3kn") / "Users";
    const std::string online_id = user.at("online_id").get<std::string>();
    if (account_id.empty() || online_id.empty() || online_id == "." || online_id == ".."
        || online_id.find('\0') != std::string::npos || fs::path(online_id).has_parent_path()
        || fs::path(online_id).has_root_name())
        throw std::runtime_error("Invalid account path");
    const fs::path directory = root / online_id;
    const fs::path path = directory / "account.json";
    fs::path backup = path;
    backup += ".previous";
    const bool existed = accounts.contains(account_id);
    try {
        if (is_unsafe_account_path("v3kn") || is_unsafe_account_path(root)
            || is_unsafe_account_path(directory) || !fs::is_directory(directory)
            || is_unsafe_account_path(path) || is_unsafe_account_path(backup))
            throw std::runtime_error("Unsafe account path");
        if (existed) {
            if (!fs::is_regular_file(path))
                throw std::runtime_error("Account file missing or invalid");
            std::ifstream input(path);
            input.exceptions(std::ios::failbit | std::ios::badbit);
            json on_disk;
            input >> on_disk;
            if (on_disk != accounts.at(account_id))
                throw std::runtime_error("Account file changed while server is running");
            fs::copy_file(path, backup, fs::copy_options::overwrite_existing);
        } else {
            if (fs::exists(path))
                throw std::runtime_error("Account file already exists");
#ifndef _WIN32
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0)
                throw std::runtime_error("Cannot create private account file");
            if (::close(fd) != 0)
                throw std::runtime_error("Cannot close new account file");
#endif
        }
        std::ofstream output(path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << user.dump(2);
        output.close();
        if (!existed)
            fs::copy_file(path, backup);
        accounts[account_id] = user;
    } catch (const std::exception &error) {
        users_failed = true;
        log("Account storage failed; restart after fixing the problem: " + std::string(error.what()));
        throw;
    }
}

void save_users(const std::string &account_id, const std::string &online_id) {
    std::lock_guard<std::mutex> lock(users_mutex);
    if (users_failed)
        throw std::runtime_error("Account storage unavailable");
    const fs::path path = fs::path("v3kn") / "users.json";
    fs::path backup = path;
    backup += ".previous";
    try {
        if (is_unsafe_account_path("v3kn") || is_unsafe_account_path(path) || is_unsafe_account_path(backup))
            throw std::runtime_error("Unsafe account index path");
        std::ifstream input(path);
        input.exceptions(std::ios::failbit | std::ios::badbit);
        json index;
        input >> index;
        if (!index.is_object() || !index.contains("users") || index["users"] != users
            || (index.contains("version") ? index.size() != 2 || index["version"] != 2 : index.size() != 1))
            throw std::runtime_error("Account index changed while server is running");
        index.erase("version");
        if (online_id.empty())
            index["users"].erase(account_id);
        else
            index["users"][account_id] = online_id;
        fs::copy_file(path, backup, fs::copy_options::overwrite_existing);
        std::ofstream output(path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << index.dump(2);
        output.close();
        users = std::move(index["users"]);
    } catch (const std::exception &error) {
        users_failed = true;
        log("Account storage failed; restart after fixing the problem: " + std::string(error.what()));
        throw;
    }
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
std::optional<std::string> generate_token() {
    std::string bytes(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), static_cast<int>(bytes.size())) != 1) {
        log("Failed to generate secure authentication token");
        return std::nullopt;
    }
    return base64_encode(bytes);
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

static std::expected<std::string, std::string> get_valid_account_id(const httplib::Request &req, const std::string &request) {
    if (users_failed)
        throw std::runtime_error("Account storage unavailable");
    const std::string token = get_token_from_request(req);
    if (token.empty()) {
        log("Missing token on request: " + request);
        return std::unexpected("ERR:MissingToken");
    }

    const std::string account_id = get_account_id_from_token(token);
    if (account_id.empty()) {
        log("Invalid token on request: " + request);
        return std::unexpected("ERR:InvalidToken");
    }

    return account_id;
}

std::expected<UserAccount, std::string> get_valid_account(const httplib::Request &req, const std::string &request) {
    const auto account_id = get_valid_account_id(req, request);
    if (!account_id)
        return std::unexpected(account_id.error());

    const std::string online_id = get_online_id_from_account_id(*account_id);
    if (online_id.empty()) {
        log("Online ID not found for account ID " + *account_id + " on request: " + request);
        return std::unexpected("ERR:OnlineIDNotFound");
    }

    return UserAccount{ *account_id, online_id };
}

std::expected<UserAccount, std::string> get_valid_target_account(const httplib::Request &req, const std::string &request, const std::string &online_id) {
    // Get target online_id from parameters
    std::string target_online_id = req.get_param_value("target_online_id");
    if (target_online_id.empty()) {
        log("Online ID " + online_id + " try to get activities with missing target online ID");
        return std::unexpected("ERR:MissingTargetOnlineID");
    }

    const std::string target_account_id = get_account_id_from_online_id(target_online_id);
    if (target_account_id.empty()) {
        log("Online ID " + online_id + " try to get activities for non-existing Account with online ID " + target_online_id);
        return std::unexpected("ERR:TargetAccountNotFound");
    }

    target_online_id = get_online_id_from_account_id(target_account_id);
    if (target_online_id.empty()) {
        log("Online ID " + online_id + " requested an account with no current online ID: " + target_account_id);
        return std::unexpected("ERR:TargetOnlineIDNotFound");
    }

    return UserAccount{ target_account_id, target_online_id };
}

std::string get_online_id_from_account_id(const std::string &account_id) {
    std::lock_guard<std::mutex> lock(users_mutex);
    auto it = users.find(account_id);
    if (it != users.end())
        return it->get<std::string>();

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

bool json_array_contains(const json &array, const json &value) {
#if defined(__cpp_lib_ranges_contains) && __cpp_lib_ranges_contains >= 202207L
    return std::ranges::contains(array, value);
#else
    return std::find(array.begin(), array.end(), value) != array.end();
#endif
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
    json user = accounts.at(account_id);

    user["last_activity"] = std::time(0);
    update_remote_addr(req, user);
    save_account(account_id, user);
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

uint64_t get_current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
