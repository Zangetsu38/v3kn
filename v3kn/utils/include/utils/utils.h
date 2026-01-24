// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <filesystem>
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
extern std::mutex token_cache_mutex;
extern std::mutex log_mutex;

// Condition variable for messages long polling
extern std::mutex messages_cv_mutex;
extern std::condition_variable messages_cv;

// Condition variable for friends long polling
extern std::mutex friends_cv_mutex;
extern std::condition_variable friends_cv;

// Token cache
extern std::unordered_map<std::string, std::string> token_cache;

// Database operations
json load_users();
void save_users(const json &db);

// Token/auth operations
std::string generate_token();
std::string get_token_from_request(const httplib::Request &req);
std::string get_npid_from_token(const std::string &token);
std::string get_valid_npid(const httplib::Request &req, const std::string &request, std::string &err);

// Crypto operations
std::vector<unsigned char> generate_salt();
std::vector<unsigned char> compute_server_hash(const std::string &client_hash, const std::vector<unsigned char> &salt);

// String operations
std::string base64_encode(const std::string &input);
std::string base64_decode(const std::string &encoded);
std::string trim_npid(std::string npid);

// Network operations
std::string get_remote_addr(const httplib::Request &req);
void update_remote_addr(const httplib::Request &req, json &user);
void update_last_activity(const httplib::Request &req, const std::string &npid);

// Logging
void log(const std::string &msg);
