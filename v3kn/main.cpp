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
            <head><title>v3kn</title></head>
            <body>
                <h1>v3kn server is running</h1>
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

    // Register all endpoints
    register_account_endpoints(v3kn);
    register_storage_endpoints(v3kn);
    register_friends_endpoints(v3kn);
    register_messages_endpoints(v3kn);

#ifndef _WIN32
    // AUTO-UPDATER THREAD
    std::thread([]() {
        while (true) {
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
                            log("Update available, Current: " + server_hash + ", Latest: " + latest_hash);
                            std::lock_guard<std::mutex> update_lock(request_mutex);
                            log("All requests finished, starting update...");
                            system("nohup ./update-v3kn.sh &");
                        }
                    }
                }
            } else {
                std::cout << "Failed to check for updates. HTTP Status: " << (res ? std::to_string(res->status) : "No Response") << std::endl;
            }
        }
    }).detach();
#endif

    // Clean old log file
    std::ofstream("v3kn.log", std::ios::trunc);

    // Pre-load token cache
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

    std::cout << "v3kn server running on port 3000..." << std::endl;
    v3kn.listen("0.0.0.0", 3000);

    return 0;
}
