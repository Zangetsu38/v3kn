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

#include "account/account.h"
#include "activity/activity.h"
#include "friend/friend.h"
#include "messages/messages.h"
#include "storage/storage.h"
#include "utils/utils.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>

//  SERVER
int main() {
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
        const std::string user_agent = req.get_header_value("User-Agent");
        if (user_agent.find("Vita3K") != std::string::npos) {
            return;
        }

        const std::string cn = req.get_header_value("CF-IPCountry");
        const std::string country = cn.empty() ? "XX" : cn;
        const std::string ip = req.get_header_value("CF-Connecting-IP");
        const std::string remote_addr = ip.empty() ? req.remote_addr : ip;
        std::string msg = req.method + " " + req.path + " from [" + country + "] " + remote_addr + ":" + std::to_string(req.remote_port);

        if (!user_agent.empty())
            msg += "\n  UA: " + user_agent;

        log(msg);
    });

    // Root endpoint
    v3kn.Get("/", [&](const httplib::Request &, httplib::Response &res) {
        std::string html = R"(
        <html>
            <head><title>Vita3K Network</title></head>
            <body>
                <h1>V3KN server is running</h1>
                <p>Welcome to the Vita3K Network server!</p>
            </body>
        </html>
        )";
        res.set_content(html, "text/html");
    });

    // Favicon
    v3kn.Get("/favicon.ico", [&](const httplib::Request &, httplib::Response &res) {
        std::ifstream file("favicon.ico", std::ios::binary);
        if (!file) {
            res.status = 404;
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        res.set_content(buffer.str(), "image/x-icon");
    });

    // Clean old log file
    std::ofstream("v3kn.log", std::ios::trunc);

    // Build account_id, online_id and token cache on startup
    {
        json db = load_users();
        if (db.contains("users")) {
            for (const auto &user : db["users"].items()) {
                const std::string account_id = user.key();
                if (!user.value().is_object()) {
                    continue;
                }

                const auto &user_data = user.value();

                // Cache the account_id -> online_id mapping
                if (user_data.contains("online_id") && !user_data["online_id"].empty()) {
                    const std::string online_id = user_data["online_id"].get<std::string>();
                    {
                        std::lock_guard<std::mutex> cache_lock(account_id_cache_mutex);
                        account_id_cache[account_id] = online_id;
                    }
                } else
                    log("User " + account_id + " has no online_id, skipping online_id cache for this user");

                // Cache the online_id -> account_id mapping
                if (user_data.contains("online_ids") && user_data["online_ids"].is_array()) {
                    for (const auto &id : user_data["online_ids"]) {
                        const std::string online_id = id.get<std::string>();
                        {
                            std::lock_guard<std::mutex> cache_lock(online_id_cache_mutex);
                            online_id_cache[online_id] = account_id;
                        }
                    }
                } else
                    log("User " + account_id + " has no online_ids array, skipping online_id cache for this user");

                // Cache the token -> account_id mapping
                if (user_data.contains("token") && !user_data["token"].empty()) {
                    const std::string token = user_data["token"].get<std::string>();
                    {
                        std::lock_guard<std::mutex> cache_lock(token_cache_mutex);
                        token_cache[token] = account_id;
                    }
                } else
                    log("User " + account_id + " has no token, skipping token cache for this user");
            }
            log("Loaded " + std::to_string(account_id_cache.size()) + " account IDs into cache");
            log("Loaded " + std::to_string(online_id_cache.size()) + " online IDs into cache");
            log("Loaded " + std::to_string(token_cache.size()) + " tokens into cache");
        }
    }

    migrate_activities_created_at_to_milliseconds();

    // Register all endpoints
    register_account_endpoints(v3kn);
    register_activity_endpoints(v3kn);
    register_storage_endpoints(v3kn);
    register_friends_endpoints(v3kn);
    register_messages_endpoints(v3kn);

#ifndef _WIN32
    // AUTO-UPDATER THREAD
    std::thread([&v3kn]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::minutes(5));
            std::string server_hash = app_hash;
            httplib::Client cli("https://api.github.com");
            auto res = cli.Get("/repos/Zangetsu38/v3kn/releases/tags/continuous");
            if (res && res->status == 200) {
                json release;
                try {
                    release = json::parse(res->body);
                } catch (...) {
                    log("Failed to parse update response JSON.");
                    continue;
                }

                std::string latest_hash;
                if (release.contains("body")) {
                    const auto &body = release["body"].get<std::string>();
                    std::string pattern = "Corresponding commit:\\s*([a-f0-9]{" + std::to_string(server_hash.size()) + "})";
                    std::regex re(pattern);
                    std::smatch match;
                    if (std::regex_search(body, match, re) && match.size() == 2) {
                        latest_hash = match[1].str();
                    }
                }

                if (!latest_hash.empty() && latest_hash != server_hash) {
                    log("Update available, Current: " + server_hash + ", Latest: " + latest_hash);

                    // Wait for all requests to finish before updating
                    std::lock_guard<std::mutex> update_lock(request_mutex);
                    log("All requests finished, starting update...");

                    const fs::path updater_path = fs::current_path() / "update-v3kn.sh";
                    log("Updater working directory: " + fs::current_path().string());
                    log("Updater script path: " + updater_path.string());
                    log("Updater script exists: " + std::string(fs::exists(updater_path) ? "yes" : "no"));

                    // Stop propetly the server
                    v3kn.stop();

                    // Spawn the updater script in the background
                    int rc = std::system("nohup ./update-v3kn.sh >./v3kn-update.log 2>&1 < /dev/null &");
                    log("Updater spawn return code: " + std::to_string(rc));
                    if (rc == -1) {
                        log("Failed to spawn updater script");
                    }

                    // Exit immediately
                    std::exit(0);
                }
            } else {
                log("Failed to check for updates. HTTP Status: " + (res ? std::to_string(res->status) : "No Response"));
            }
        }
    }).detach();
#endif

    reload_stitles_cache();
    log("Loaded " + std::to_string(get_stitles_cache_size()) + " titles into cache");

    log("Starting v3kn server version " + std::string(app_hash) + " on port 3000...");
    v3kn.listen("0.0.0.0", 3000);

    return 0;
}
