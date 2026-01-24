// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Constants
constexpr uint64_t DEFAULT_QUOTA_TOTAL = 50 * 1024 * 1024; // 50 MB

// Mutexes
extern std::mutex account_mutex;
extern std::mutex request_mutex;
extern std::mutex stitles_cache_mutex;
extern std::mutex log_mutex;

// Caches mutexes
extern std::mutex token_cache_mutex;
extern std::mutex online_id_cache_mutex;
extern std::mutex account_id_cache_mutex;

// Condition variable for messages long polling
extern std::mutex messages_cv_mutex;
extern std::condition_variable messages_cv;

// Condition variable for friends long polling
extern std::mutex friends_cv_mutex;
extern std::condition_variable friends_cv;

// Shared poll events storage
void load_poll_events_from_disk();
bool update_poll_events(const std::string &account_id, const std::function<void(json &events)> &updater);
json pop_poll_events(const std::string &account_id);
void cleanup_old_poll_events(int64_t max_age_seconds);

// Token cache to speed up token -> account_id lookups (not persisted to disk, built on server start and updated on demand)
extern std::unordered_map<std::string, std::string> token_cache; // token -> account_id

// Online IDs cache to speed up lookups (not persisted to disk, built on server start and updated on demand)
extern std::unordered_map<std::string, std::string> online_id_cache; // online_id -> account_id

// Account ID cache to speed up lookups (not persisted to disk, built on server start and updated on demand)
extern std::unordered_map<std::string, std::string> account_id_cache; // account_id -> online_id

struct UserAccount {
    std::string account_id;
    std::string online_id;
};

// Database operations
json load_profile(const std::string &online_id);
void save_profile(const std::string &online_id, const json &profile);
json load_users();
void save_users(const json &db);
json load_stitles();
void save_stitles(const json &db);
void reload_stitles_cache();
bool has_stitle_info(const std::string &titleid);
void update_stitle_info(const std::string &titleid, const json &names);
std::string get_stitle_name(const std::string &titleid, const std::string &language);
size_t get_stitles_cache_size();

// Token/auth operations
std::string generate_token();
std::string get_token_from_request(const httplib::Request &req);
std::string get_account_id_from_token(const std::string &token);
std::string get_online_id_from_account_id(const std::string &account_id);
std::string get_account_id_from_online_id(const std::string &online_id);
std::optional<UserAccount> get_valid_account(const httplib::Request &req, const std::string &request, std::string &err);
std::optional<UserAccount> get_valid_target_account(const httplib::Request &req, const std::string &request, std::string &err, const std::string &online_id);

// Crypto operations
std::vector<unsigned char> generate_salt();
std::vector<unsigned char> compute_server_hash(const std::string &client_hash, const std::vector<unsigned char> &salt);

// String operations
std::string base64_encode(const std::string &input);
std::string base64_decode(const std::string &encoded);
std::string lowercase_online_id(std::string online_id);
std::string trim_online_id(std::string online_id);

// Network operations
std::string get_remote_addr(const httplib::Request &req);
void update_remote_addr(const httplib::Request &req, json &user);
void update_last_activity(const httplib::Request &req, const std::string &account_id);

// Logging
void log(const std::string &msg);

// Time operations
uint64_t get_current_time_ms();
