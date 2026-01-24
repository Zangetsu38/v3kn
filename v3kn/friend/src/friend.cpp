// v3knr project
// Copyright (C) 2026 Vita3K team

#include <activity/activity.h>
#include <friend/friend.h>
#include <utils/utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "pugixml.hpp"

// In-memory online status: account_id -> last presence timestamp (not persisted to disk)
static std::unordered_map<std::string, int64_t> online_users;
static std::unordered_map<std::string, int64_t> last_status_change; // account_id -> timestamp of last online/offline change
static std::unordered_map<std::string, std::string> online_now_playing; // account_id -> now playing string
static std::unordered_map<std::string, std::vector<json>> friend_events; // account_id -> list of change events
static std::unordered_map<std::string, std::string> presence_status; // account_id -> online/offline/not_available
static std::unordered_set<std::string> pending_online_poll; // account_id -> waiting to poll on online
static std::mutex online_users_mutex;
static std::mutex friend_events_mutex;
static std::mutex friend_events_file_mutex;
static std::condition_variable online_monitor_cv;
static std::atomic<bool> monitor_running{ true };
static json load_friends(const std::string &online_id, const std::string &group);
static json load_friends_data(const std::string &online_id);

static void load_friend_events_from_disk() {
    std::ifstream f("v3kn/events.json");
    if (!f.is_open())
        return;

    json data;
    f >> data;
    if (!data.is_object())
        return;

    friend_events.clear();
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (it.value().is_array()) {
            std::string account_id = it.key();
            if (get_online_id_from_account_id(account_id).empty()) {
                const auto resolved_account_id = get_account_id_from_online_id(it.key());
                if (!resolved_account_id.empty())
                    account_id = resolved_account_id;
            }

            auto events = it.value().get<std::vector<json>>();
            for (auto &event : events) {
                if (!event.contains("account_id") && event.contains("online_id") && event["online_id"].is_string()) {
                    const auto target_account_id = get_account_id_from_online_id(event["online_id"].get<std::string>());
                    if (!target_account_id.empty()) {
                        event.erase("online_id");
                        event["account_id"] = target_account_id;
                    }
                }
            }

            friend_events[account_id] = std::move(events);
        }
    }
}

static void save_friend_events_to_disk() {
    std::lock_guard<std::mutex> file_lock(friend_events_file_mutex);
    json data = json::object();
    for (const auto &[account_id, events] : friend_events) {
        data[account_id] = events;
    }
    std::ofstream f("v3kn/events.json");
    f << data.dump(2);
}

struct FriendPollSignal {
    std::condition_variable cv;
    size_t waiters = 0;
};

static std::unordered_map<std::string, std::shared_ptr<FriendPollSignal>> friend_poll_signals;
static std::mutex friend_poll_signals_mutex;

static std::shared_ptr<FriendPollSignal> get_friend_poll_signal(const std::string &account_id) {
    std::lock_guard<std::mutex> lock(friend_poll_signals_mutex);
    auto &signal = friend_poll_signals[account_id];
    if (!signal) {
        signal = std::make_shared<FriendPollSignal>();
    }
    return signal;
}

static void push_friend_status_event(const std::string &account_id, const std::string &target_account_id, bool is_online) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    json event;
    event["type"] = "status_changed";
    event["account_id"] = target_account_id;
    event["status"] = is_online ? "online" : "offline";
    event["at"] = std::time(0);
    friend_events[account_id].push_back(event);
}

static void notify_friend_poll(const std::string &account_id) {
    std::shared_ptr<FriendPollSignal> signal;
    {
        std::lock_guard<std::mutex> lock(friend_poll_signals_mutex);
        auto it = friend_poll_signals.find(account_id);
        if (it != friend_poll_signals.end()) {
            signal = it->second;
        }
    }
    if (signal) {
        signal->cv.notify_one();
    }
}

static void push_status_event_to_friends(const std::string &online_id, bool is_online) {
    json user_friends = load_friends(online_id, "friends");
    std::lock_guard<std::mutex> lock(online_users_mutex);
    for (const auto &f : user_friends) {
        if (!f.contains("account_id"))
            continue;
        const std::string friend_account_id = f["account_id"].get<std::string>();
        if (!online_users.contains(friend_account_id))
            continue;
        const auto target_account_id = get_account_id_from_online_id(online_id);
        push_friend_status_event(friend_account_id, target_account_id, is_online);
        notify_friend_poll(friend_account_id);
    }
}

struct FriendPollWaiter {
    std::string account_id;
    std::shared_ptr<FriendPollSignal> signal;

    FriendPollWaiter(const std::string &account_id, std::shared_ptr<FriendPollSignal> signal)
        : account_id(account_id)
        , signal(std::move(signal)) {
        std::lock_guard<std::mutex> lock(friend_poll_signals_mutex);
        ++this->signal->waiters;
    }

    ~FriendPollWaiter() {
        std::lock_guard<std::mutex> lock(friend_poll_signals_mutex);
        if (signal->waiters > 0) {
            --signal->waiters;
        }
        if (signal->waiters == 0) {
            auto it = friend_poll_signals.find(account_id);
            if (it != friend_poll_signals.end() && it->second == signal) {
                friend_poll_signals.erase(it);
            }
        }
    }
};

// Background thread that monitors online users and detects timeouts
static void monitor_online_users() {
    const int64_t timeout_threshold = 30; // 30 seconds

    while (monitor_running) {
        std::unique_lock<std::mutex> lock(online_users_mutex);

        // If no online users, wait indefinitely until someone comes online
        if (online_users.empty()) {
            online_monitor_cv.wait(lock, [] { return !online_users.empty() || !monitor_running; });
            if (!monitor_running)
                break;
        }

        // Wait for 30 seconds or until notified
        online_monitor_cv.wait_for(lock, std::chrono::seconds(timeout_threshold));
        if (!monitor_running)
            break;

        // Check for timeouts
        const int64_t now = std::time(0);
        std::vector<std::string> timed_out_users;

        for (auto it = online_users.begin(); it != online_users.end();) {
            int64_t last_presence = it->second;
            if ((now - last_presence) > timeout_threshold) {
                const std::string account_id = it->first;
                timed_out_users.push_back(account_id);
                online_now_playing.erase(account_id);
                presence_status.erase(account_id);
                pending_online_poll.erase(account_id);
                it = online_users.erase(it);
            } else {
                ++it;
            }
        }

        // Notify polls about status changes
        if (!timed_out_users.empty()) {
            const int64_t change_time = std::time(0);

            // Mark status change timestamp for timed out users
            for (const auto &account_id : timed_out_users) {
                last_status_change[account_id] = change_time;
            }

            lock.unlock(); // Release lock before notifications

            for (const auto &account_id : timed_out_users) {
                const std::string online_id = get_online_id_from_account_id(account_id);
                if (!online_id.empty())
                    push_status_event_to_friends(online_id, false);
                log("User timeout detected: " + (!online_id.empty() ? online_id : account_id) + " -> offline");
            }
        }

        // Cleanup old status change entries (older than 7 days)
        const int64_t status_cleanup_age = 604800; // 7 days
        for (auto it = last_status_change.begin(); it != last_status_change.end();) {
            if ((now - it->second) > status_cleanup_age) {
                it = last_status_change.erase(it);
            } else {
                ++it;
            }
        }

        // Cleanup old friend events (older than 7 days)
        {
            std::lock_guard<std::mutex> events_lock(friend_events_mutex);
            for (auto it = friend_events.begin(); it != friend_events.end();) {
                auto &events = it->second;
                events.erase(
                    std::remove_if(events.begin(), events.end(),
                        [now, status_cleanup_age](const json &e) {
                            return e.contains("at") && (now - e["at"].get<int64_t>()) > status_cleanup_age;
                        }),
                    events.end());
                if (events.empty()) {
                    it = friend_events.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

void register_friends_endpoints(httplib::Server &server) {
    load_friend_events_from_disk();
    server.Post("/v3kn/friends/add", handle_friend_add);
    server.Post("/v3kn/friends/accept", handle_friend_accept);
    server.Post("/v3kn/friends/reject", handle_friend_reject);
    server.Post("/v3kn/friends/remove", handle_friend_remove);
    server.Post("/v3kn/friends/cancel", handle_friend_cancel);
    server.Post("/v3kn/friends/block", handle_friend_block);
    server.Post("/v3kn/friends/unblock", handle_friend_unblock);
    server.Post("/v3kn/friends/presence", handle_friend_presence);
    server.Get("/v3kn/friends/list", handle_friend_list);
    server.Get("/v3kn/friends/profile", handle_friend_profile);
    server.Get("/v3kn/friends/poll", handle_friend_poll);
    server.Get("/v3kn/friends/search", handle_friend_search);

    // Start the online users monitoring thread
    static std::thread monitor_thread(monitor_online_users);
    monitor_thread.detach();
}

// Helper: Get friends file path
static std::string get_friends_path(const std::string &online_id) {
    fs::path friends_dir = fs::path("v3kn") / "Users" / online_id;
    fs::create_directories(friends_dir);
    return (friends_dir / "friends.json").string();
}

// Helper: Push a friend event for a user
static void push_friend_event(const std::string &account_id, const std::string &type, const std::string &target_account_id) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    json event;
    event["type"] = type;
    event["account_id"] = target_account_id;
    event["at"] = std::time(0);
    friend_events[account_id].push_back(event);
    save_friend_events_to_disk();
}

static void remove_friend_event(const std::string &account_id, const std::string &type, const std::string &target_account_id) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    auto it = friend_events.find(account_id);
    if (it == friend_events.end())
        return;
    const std::string target_online_id = get_online_id_from_account_id(target_account_id);
    auto &events = it->second;
    events.erase(
        std::remove_if(events.begin(), events.end(),
            [&type, &target_account_id, &target_online_id](const json &event) {
                return event.value("type", "") == type && (event.value("account_id", "") == target_account_id || event.value("online_id", "") == target_online_id);
            }),
        events.end());
    if (events.empty()) {
        friend_events.erase(it);
    }
    save_friend_events_to_disk();
}

// Helper: Get friend events since a timestamp for a user
static json get_friend_events_since(const std::string &account_id, int64_t since) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    json result = json::array();
    auto it = friend_events.find(account_id);
    if (it != friend_events.end()) {
        result = it->second;
        friend_events.erase(it);
        save_friend_events_to_disk();
    }
    return result;
}

// Helper: Load friends data from file
static json load_friends(const std::string &online_id, const std::string &group) {
    std::string path = get_friends_path(online_id);
    std::ifstream f(path);

    json friends_data;
    if (!f.is_open()) {
        friends_data["friends"] = json::array();
        friends_data["friend_requests"] = json::object();
        friends_data["friend_requests"]["sent"] = json::array();
        friends_data["friend_requests"]["received"] = json::array();
        friends_data["players_blocked"] = json::array();
        if (group == "friends")
            return friends_data["friends"];
        if (group == "friend_requests")
            return friends_data["friend_requests"];
        if (group == "players_blocked")
            return friends_data["players_blocked"];
        return friends_data;
    }

    f >> friends_data;

    if (!friends_data.contains("friends") || !friends_data["friends"].is_array())
        friends_data["friends"] = json::array();
    if (!friends_data.contains("friend_requests") || !friends_data["friend_requests"].is_object())
        friends_data["friend_requests"] = json::object();
    if (!friends_data["friend_requests"].contains("sent") || !friends_data["friend_requests"]["sent"].is_array())
        friends_data["friend_requests"]["sent"] = json::array();
    if (!friends_data["friend_requests"].contains("received") || !friends_data["friend_requests"]["received"].is_array())
        friends_data["friend_requests"]["received"] = json::array();
    if (!friends_data.contains("players_blocked") || !friends_data["players_blocked"].is_array())
        friends_data["players_blocked"] = json::array();

    if (group == "friends")
        return friends_data["friends"];
    if (group == "friend_requests")
        return friends_data["friend_requests"];
    if (group == "players_blocked")
        return friends_data["players_blocked"];

    return json::object();
}

static json load_friends_data(const std::string &online_id) {
    json friends_data;
    friends_data["friends"] = load_friends(online_id, "friends");
    friends_data["friend_requests"] = load_friends(online_id, "friend_requests");
    friends_data["players_blocked"] = load_friends(online_id, "players_blocked");
    return friends_data;
}

// Helper: Save friends data to file
static void save_friends(const std::string &online_id, const json &friends_data) {
    std::string path = get_friends_path(online_id);
    std::ofstream f(path);
    f << friends_data.dump(2);
}

void migrate_friends_npid_to_account_id() {
    const auto migrate_npid_list = [&](json &list, const std::string &name) {
        if (!list.is_array())
            return;

        for (auto &entry : list) {
            if (entry.contains("npid") && entry["npid"].is_string()) {
                const auto npid = entry["npid"].get<std::string>();
                const auto account_id = get_account_id_from_online_id(npid);

                if (account_id.empty()) {
                    log("Failed to find account ID for " + name + " entry npid " + npid + ", skipping");
                    continue;
                }

                entry.erase("npid");
                entry["account_id"] = account_id;
            } else
                log("No npid found for " + name + " entry, skipping");
        }
    };

    {
        std::lock_guard<std::mutex> lock(account_mutex);
        json db = load_users();

        for (const auto &[_, user] : db["users"].items()) {
            const auto online_id = user["online_id"].get<std::string>();
            std::string path = get_friends_path(online_id);
            if (!fs::exists(path))
                continue;

            {
                std::lock_guard<std::mutex> friends_lock(online_users_mutex);
                json friends = load_friends_data(online_id);

                // Friends
                if (friends.contains("friends"))
                    migrate_npid_list(friends["friends"], "friends");

                // Friend requests
                if (friends.contains("friend_requests") && friends["friend_requests"].is_object()) {
                    auto &req = friends["friend_requests"];
                    if (req.contains("sent"))
                        migrate_npid_list(req["sent"], "friend_requests.sent");

                    if (req.contains("received"))
                        migrate_npid_list(req["received"], "friend_requests.received");
                }

                // Blocked players
                if (friends.contains("players_blocked"))
                    migrate_npid_list(friends["players_blocked"], "players_blocked");

                save_friends(online_id, friends);
            }

            log("Migrated friends for online ID " + online_id);
        }
    }
}

void migrate_friend_events_npid_to_account_id() {
    std::lock_guard<std::mutex> file_lock(friend_events_file_mutex);
    std::ifstream f("v3kn/events.json");
    if (!f.is_open())
        return;

    json data;
    f >> data;
    if (!data.is_object())
        return;

    bool modified = false;
    json new_data = json::object();

    for (auto &pair : data.items()) {
        const std::string owner_key = pair.key();
        json &events = pair.value();

        // On ne traite que les tableaux d'événements
        if (!events.is_array()) {
            log("Skipping invalid friend events entry for owner " + owner_key + " (not an array)");
            continue;
        }

        // Convertir owner NPID -> account_id
        const std::string owner_account_id = get_account_id_from_online_id(owner_key);
        const std::string new_owner_key = owner_account_id.empty() ? owner_key : owner_account_id;

        if (!owner_account_id.empty() && owner_key != new_owner_key) {
            modified = true;
            log("Migrating friend events owner " + owner_key + " -> " + new_owner_key);
        }

        // Migration interne des events
        for (auto &event : events) {
            if (!event.is_object())
                continue;

            const std::string account_id = event.value("account_id", "");
            if (account_id.empty()) {
                const std::string npid = event["npid"].get<std::string>();
                const std::string target_account_id = get_account_id_from_online_id(npid);

                if (!target_account_id.empty()) {
                    event.erase("npid");
                    event["account_id"] = target_account_id;
                    modified = true;
                } else {
                    log("Failed to migrate event target npid " + npid + " (no account found)");
                }
            } else {
                const std::string online_id = get_online_id_from_account_id(account_id);
                log("Event already has account_id " + account_id + " -> " + online_id + ", skipping");
            }
        }

        // On place les events migrés dans la nouvelle map
        new_data[new_owner_key] = events;
    }

    if (!modified)
        return;

    std::ofstream out("v3kn/events.json");
    out << new_data.dump(2);

    log("Migrated friend events from NPID to account ID");
}

// Helper: Find friend by online_id in array of objects
static bool has_friend(const json &friends, const std::string &account_id) {
    for (const auto &f : friends) {
        if (f.is_object() && f.contains("account_id") && f["account_id"] == account_id)
            return true;
    }
    return false;
}

// Helper: Remove friend by online_id from array of objects
static void remove_friend(json &friends, const std::string &account_id) {
    for (auto it = friends.begin(); it != friends.end(); ++it) {
        if (it->is_object() && it->contains("account_id") && (*it)["account_id"] == account_id) {
            friends.erase(it);
            return;
        }
    }
}

static bool replace_account_id_with_online_id(json &entry) {
    if (!entry.is_object())
        return false;
    if (!entry.contains("account_id") || !entry["account_id"].is_string())
        return entry.contains("online_id") && entry["online_id"].is_string();

    const std::string online_id = get_online_id_from_account_id(entry["account_id"].get<std::string>());
    if (online_id.empty())
        return false;

    entry.erase("account_id");
    entry["online_id"] = online_id;
    return true;
}

static json convert_friend_entries_for_client(const json &entries) {
    json result = json::array();
    if (!entries.is_array())
        return result;

    for (const auto &entry : entries) {
        json client_entry = entry;
        if (replace_account_id_with_online_id(client_entry))
            result.push_back(client_entry);
    }

    return result;
}

// Helper: Check if user is online (in the online_users map)
static bool is_user_online(const std::string &account_id) {
    std::lock_guard<std::mutex> lock(online_users_mutex);
    const auto online_id = get_online_id_from_account_id(account_id);
    return online_users.contains(online_id);
}

static void calculate_trophy_level(int64_t points, int &level, int &progress) {
    struct LevelRange {
        int start_level;
        int end_level;
        int points_per_level;
        int64_t start_points;
    };

    static const std::array<LevelRange, 10> ranges = {
        LevelRange{ 1, 99, 60, 0 },
        LevelRange{ 100, 199, 90, 5940 },
        LevelRange{ 200, 299, 450, 14940 },
        LevelRange{ 300, 399, 900, 59940 },
        LevelRange{ 400, 499, 1350, 149940 },
        LevelRange{ 500, 599, 1800, 284940 },
        LevelRange{ 600, 699, 2250, 464940 },
        LevelRange{ 700, 799, 2700, 689940 },
        LevelRange{ 800, 899, 3150, 959940 },
        LevelRange{ 900, 999, 3600, 1274940 }
    };

    if (points < 0)
        points = 0;

    for (const auto &range : ranges) {
        const int64_t range_points = static_cast<int64_t>(range.end_level - range.start_level + 1) * range.points_per_level;
        if (points < range.start_points + range_points) {
            const int64_t offset = points - range.start_points;
            const int64_t level_offset = offset / range.points_per_level;
            const int64_t progress_points = offset % range.points_per_level;
            level = range.start_level + static_cast<int>(level_offset);
            progress = static_cast<int>((progress_points * 100) / range.points_per_level);
            return;
        }
    }

    level = 999;
    progress = 100;
}

static json load_trophies_summary(const std::string &online_id) {
    json summary = json::object();
    summary["level"] = 1;
    summary["progress"] = 0;
    summary["total"] = 0;
    summary["bronze"] = 0;
    summary["silver"] = 0;
    summary["gold"] = 0;
    summary["platinum"] = 0;

    const fs::path trophies_path = fs::path("v3kn") / "Users" / online_id / "trophy" / "trophies.xml";
    pugi::xml_document doc;
    if (!doc.load_file(trophies_path.string().c_str()))
        return summary;

    const auto root = doc.child("trophies");
    if (!root)
        return summary;

    int64_t unlocked_count = 0;
    int64_t platinum = 0;
    int64_t gold = 0;
    int64_t silver = 0;
    int64_t bronze = 0;

    if (!root.child("trophy").empty()) {
        for (const auto &trophy : root.children("trophy")) {
            unlocked_count += trophy.attribute("unlocked_count").as_int();
            platinum += trophy.attribute("platinum").as_int();
            gold += trophy.attribute("gold").as_int();
            silver += trophy.attribute("silver").as_int();
            bronze += trophy.attribute("bronze").as_int();
        }
    } else {
        for (const auto &np : root.children("np")) {
            const pugi::xml_node progress = np.child("progress");
            unlocked_count += progress.attribute("unlocked_count").as_int();
            platinum += progress.attribute("platinum").as_int();
            gold += progress.attribute("gold").as_int();
            silver += progress.attribute("silver").as_int();
            bronze += progress.attribute("bronze").as_int();
        }
    }

    const int64_t total = unlocked_count > 0 ? unlocked_count : bronze + silver + gold + platinum;
    const int64_t points = (bronze * 15) + (silver * 30) + (gold * 90) + (platinum * 300);

    int level = 1;
    int progress = 0;
    calculate_trophy_level(points, level, progress);

    summary["level"] = level;
    summary["progress"] = progress;
    summary["total"] = total;
    summary["platinum"] = platinum;
    summary["gold"] = gold;
    summary["bronze"] = bronze;
    summary["silver"] = silver;

    return summary;
}

static void fill_presence_fields(json &status_obj, const std::string &account_id, bool include_last_activity, const std::string &language) {
    std::lock_guard<std::mutex> lock(online_users_mutex);
    const auto status_it = presence_status.find(account_id);
    const std::string status = (status_it != presence_status.end()) ? status_it->second : "offline";
    const bool is_online = status != "offline";
    std::string now_playing = "";
    if (is_online) {
        auto now_playing_it = online_now_playing.find(account_id);
        if (now_playing_it != online_now_playing.end())
            now_playing = get_stitle_name(now_playing_it->second, language);
    }
    status_obj["status"] = status;
    status_obj["now_playing"] = now_playing;
    if (include_last_activity) {
        status_obj["last_activity"] = last_status_change.contains(account_id) ? last_status_change[account_id] : 0;
    }
}

void handle_friend_add(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friend add request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend add request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    if (online_id == target_online_id) {
        log("Cannot add yourself as friend: " + online_id);
        res.set_content("ERR:CannotAddYourself", "text/plain");
        return;
    }

    if (account_id == target_account_id) {
        log("Cannot add yourself as friend (account ID match): " + online_id);
        res.set_content("ERR:CannotAddYourself", "text/plain");
        return;
    }

    json user_friends = load_friends_data(online_id);
    json target_friends = load_friends_data(target_online_id);

    const bool is_blocked_by_target = has_friend(target_friends["players_blocked"], account_id);

    if (has_friend(user_friends["friends"], target_account_id)) {
        log("Already friends: " + online_id + " and " + target_online_id);
        res.set_content("ERR:AlreadyFriends", "text/plain");
        return;
    }

    if (has_friend(user_friends["friend_requests"]["sent"], target_account_id)) {
        log("Friend request already sent from " + online_id + " to " + target_online_id);
        res.set_content("ERR:RequestAlreadySent", "text/plain");
        return;
    }

    const auto add_friend_request = [](const std::string &target_account_id, const std::string &target_online_id, json &target_friends, const std::string &type) {
        if (!has_friend(target_friends["friend_requests"][type], target_account_id)) {
            json req_obj;
            req_obj["account_id"] = target_account_id;
            req_obj["sent_at"] = std::time(0);
            target_friends["friend_requests"][type].push_back(req_obj);
            save_friends(target_online_id, target_friends);
        }
    };

    if (is_blocked_by_target) {
        add_friend_request(account_id, target_online_id, target_friends, "sent");

        log("Friend request silently stored from " + online_id + " to blocked target " + target_online_id);
        res.set_content("OK:RequestSent", "text/plain");
        return;
    }

    const bool has_received_request = has_friend(user_friends["friend_requests"]["received"], target_account_id);
    const bool has_target_sent_request = has_friend(target_friends["friend_requests"]["sent"], account_id);
    if (has_received_request || has_target_sent_request) {
        // Auto-accept
        if (has_received_request) {
            remove_friend(user_friends["friend_requests"]["received"], target_account_id);
        }
        if (has_target_sent_request) {
            remove_friend(target_friends["friend_requests"]["sent"], account_id);
        }

        json friend_obj;
        friend_obj["account_id"] = target_account_id;
        friend_obj["since"] = std::time(0);
        user_friends["friends"].push_back(friend_obj);

        json friend_obj2;
        friend_obj2["account_id"] = account_id;
        friend_obj2["since"] = std::time(0);
        target_friends["friends"].push_back(friend_obj2);

        save_friends(online_id, user_friends);
        save_friends(target_online_id, target_friends);

        // Create friendship established activities for both users
        create_friendship_established_activity(account_id, target_account_id);

        log("Auto-accepted friend request: " + online_id + " <-> " + target_online_id);
        res.set_content("OK:FriendAdded", "text/plain");
        return;
    }

    // Send friend request
    add_friend_request(target_account_id, online_id, user_friends, "sent");

    add_friend_request(account_id, target_online_id, target_friends, "received");

    push_friend_event(target_account_id, "friends_request_received", account_id);

    log("Friend request sent from " + online_id + " to " + target_online_id);

    notify_friend_poll(target_account_id);
    res.set_content("OK:RequestSent", "text/plain");
}

void handle_friend_accept(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friend accept request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend accept request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    if (account_id == target_account_id) {
        log("Cannot accept friend request from yourself (account ID match): " + online_id);
        res.set_content("ERR:CannotAcceptYourself", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_account_id)) {
        log("Friend accept request to non-existing account ID " + target_account_id + " by " + online_id);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(online_id);
    json target_friends = load_friends_data(target_online_id);

    if (!has_friend(user_friends["friend_requests"]["received"], target_account_id)) {
        log("No friend request from " + target_account_id + " to accept by " + account_id);
        res.set_content("ERR:NoRequestFound", "text/plain");
        return;
    }

    // Accept request
    const auto accept_friend_request = [](json &friends, const std::string &account_id, const std::string &target_account_id, const std::string &online_id, const std::string &type) {
        remove_friend(friends["friend_requests"][type], target_account_id);
        json friend_obj;
        friend_obj["account_id"] = target_account_id;
        friend_obj["since"] = std::time(0);
        friends["friends"].push_back(friend_obj);
        save_friends(online_id, friends);
    };

    accept_friend_request(user_friends, account_id, target_account_id, online_id, "received");
    accept_friend_request(target_friends, target_account_id, account_id, target_online_id, "sent");

    // Create friendship established activities for both users
    create_friendship_established_activity(account_id, target_account_id);

    log("Friend request accepted: " + online_id + " <-> " + target_online_id);
    res.set_content("OK:FriendAdded", "text/plain");
}

void handle_friend_reject(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friend reject request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend reject request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    json user_friends = load_friends_data(online_id);
    json target_friends = load_friends_data(target_online_id);

    if (!has_friend(user_friends["friend_requests"]["received"], target_account_id)) {
        log("No friend request from " + target_online_id + " to reject by " + online_id);
        res.set_content("ERR:NoRequestFound", "text/plain");
        return;
    }

    // Reject request
    remove_friend(user_friends["friend_requests"]["received"], target_account_id);
    remove_friend(target_friends["friend_requests"]["sent"], account_id);

    save_friends(online_id, user_friends);
    save_friends(target_online_id, target_friends);

    log("Friend request rejected: " + target_online_id + " -> " + online_id);

    res.set_content("OK:RequestRejected", "text/plain");
}

void handle_friend_remove(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friend remove request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend remove request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    if (account_id == target_account_id) {
        log("Cannot remove yourself as friend (account ID match): " + online_id);
        res.set_content("ERR:CannotRemoveYourself", "text/plain");
        return;
    }

    if (target_online_id.empty()) {
        log("Friend remove request to non-existing online_id " + target_online_id + " by " + online_id);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(online_id);
    json target_friends = load_friends_data(target_online_id);

    if (!has_friend(user_friends["friends"], target_account_id)) {
        log("Not friends: " + online_id + " and " + target_online_id);
        res.set_content("ERR:NotFriends", "text/plain");
        return;
    }

    // Remove friendship
    remove_friend(user_friends["friends"], target_account_id);
    remove_friend(target_friends["friends"], account_id);

    save_friends(online_id, user_friends);
    save_friends(target_online_id, target_friends);

    log("Friendship removed: " + online_id + " <-> " + target_online_id);

    res.set_content("OK:FriendRemoved", "text/plain");
}

void handle_friend_cancel(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friend cancel request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend cancel request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;
    if (target_account_id.empty()) {
        log("Friend cancel request to user with no account ID " + target_online_id + " by " + online_id);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(online_id);
    json target_friends = load_friends_data(target_online_id);

    if (!has_friend(user_friends["friend_requests"]["sent"], target_account_id)) {
        log("No friend request to cancel from " + online_id + " to " + target_online_id);
        res.set_content("ERR:NoRequestFound", "text/plain");
        return;
    }

    // Cancel the friend request
    remove_friend(user_friends["friend_requests"]["sent"], target_account_id);
    remove_friend(target_friends["friend_requests"]["received"], account_id);

    save_friends(online_id, user_friends);
    save_friends(target_online_id, target_friends);

    remove_friend_event(target_account_id, "friends_request_received", account_id);

    log("Friend request cancelled: " + online_id + " -> " + target_online_id);

    res.set_content("OK:RequestCancelled", "text/plain");
}

void handle_friend_block(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friend block request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend block request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    if (account_id == target_account_id) {
        log("Cannot block yourself: " + online_id);
        res.set_content("ERR:CannotBlockYourself", "text/plain");
        return;
    }

    json user_friends = load_friends_data(online_id);
    json target_friends = load_friends_data(target_online_id);

    if (!has_friend(user_friends["players_blocked"], target_account_id)) {
        json block_obj;
        block_obj["account_id"] = target_account_id;
        block_obj["blocked_at"] = std::time(0);
        user_friends["players_blocked"].push_back(block_obj);
    }

    const bool is_friends = has_friend(user_friends["friends"], target_account_id);
    const bool user_sent_request = has_friend(user_friends["friend_requests"]["sent"], target_account_id);
    const bool target_sent_request = has_friend(target_friends["friend_requests"]["sent"], account_id);

    if (is_friends) {
        remove_friend(user_friends["friends"], target_account_id);
        remove_friend(target_friends["friends"], account_id);
    }

    if (user_sent_request) {
        remove_friend(user_friends["friend_requests"]["sent"], target_account_id);
        remove_friend(target_friends["friend_requests"]["received"], account_id);
    }

    if (target_sent_request) {
        remove_friend(user_friends["friend_requests"]["received"], target_account_id);
    }

    save_friends(online_id, user_friends);
    if (is_friends || user_sent_request) {
        save_friends(target_online_id, target_friends);
    }

    log("Player blocked: " + online_id + " -> " + target_online_id);
    res.set_content("OK:PlayerBlocked", "text/plain");
}

void handle_friend_unblock(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friend unblock request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend unblock request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    json user_friends = load_friends_data(online_id);
    json target_friends = load_friends_data(target_online_id);

    remove_friend(user_friends["players_blocked"], target_account_id);

    const bool target_sent_request = has_friend(target_friends["friend_requests"]["sent"], account_id);
    if (target_sent_request && !has_friend(user_friends["friend_requests"]["received"], target_account_id)) {
        json req_obj;
        req_obj["account_id"] = target_account_id;
        req_obj["received_at"] = std::time(0);
        user_friends["friend_requests"]["received"].push_back(req_obj);
        notify_friend_poll(account_id);
    }

    save_friends(online_id, user_friends);

    log("Player unblocked: " + online_id + " -> " + target_online_id);
    res.set_content("OK:PlayerUnblocked", "text/plain");
}

void handle_friend_list(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friends list request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const std::string group = req.get_param_value("group");
    if (group.empty()) {
        log("Missing group parameter on friends list request for online ID " + online_id);
        res.set_content("ERR:MissingGroup", "text/plain");
        return;
    }

    const std::string language = req.get_param_value("sys_lang");
    if (language.empty()) {
        log("Missing sys_lang parameter on friends list request for online ID " + online_id);
        res.set_content("ERR:MissingLanguage", "text/plain");
        return;
    }

    json response = json::object();
    if (group == "friends") {
        json friends = load_friends(online_id, "friends");
        json enriched_friends = json::array();
        for (const auto &f : friends) {
            if (!f.contains("account_id"))
                continue;
            json entry = f;
            const std::string friend_account_id = f["account_id"].get<std::string>();
            const std::string friend_online_id = get_online_id_from_account_id(friend_account_id);
            if (friend_online_id.empty())
                continue;
            fill_presence_fields(entry, friend_account_id, false, language);
            entry.erase("account_id");
            entry["online_id"] = friend_online_id;
            entry["trophy_level"] = load_trophies_summary(friend_online_id)["level"];
            enriched_friends.push_back(entry);
        }
        response["friends"] = enriched_friends;
        json self_entry = json::object();
        self_entry["online_id"] = online_id;
        self_entry["since"] = 0;
        fill_presence_fields(self_entry, account_id, false, language);
        self_entry["trophy_level"] = load_trophies_summary(online_id)["level"];
        response["self"] = self_entry;
    } else if (group == "friend_requests") {
        json requests = load_friends(online_id, "friend_requests");
        response["friend_requests"] = json::object();
        response["friend_requests"]["sent"] = convert_friend_entries_for_client(requests["sent"]);
        response["friend_requests"]["received"] = convert_friend_entries_for_client(requests["received"]);
    } else if (group == "players_blocked") {
        response["players_blocked"] = convert_friend_entries_for_client(load_friends(online_id, "players_blocked"));
    } else {
        res.set_content("ERR:InvalidGroup", "text/plain");
        return;
    }

    log("Friends list requested by " + online_id + " (" + group + ")");
    res.set_content(response.dump(), "application/json");
}

void handle_friend_profile(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "friends profile request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "friend profile request", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    const std::string language = req.get_param_value("sys_lang");
    if (language.empty()) {
        log("Missing sys_lang parameter on friend profile request for online ID " + online_id);
        res.set_content("ERR:MissingLanguage", "text/plain");
        return;
    }

    json response = json::object();
    response["online_id"] = target_online_id;
    response["friends"] = json::array();
    response["trophies"] = load_trophies_summary(target_online_id);

    json friends = load_friends(online_id, "friends");
    json requests = load_friends(online_id, "friend_requests");
    json blocked = load_friends(online_id, "players_blocked");

    if (has_friend(blocked, target_account_id)) {
        response["relationship"] = "blocked";
    } else if (has_friend(friends, target_account_id)) {
        response["relationship"] = "friends";
        response["friends"] = convert_friend_entries_for_client(load_friends(target_online_id, "friends"));
        fill_presence_fields(response, target_account_id, false, language);
    } else if (has_friend(requests["sent"], target_account_id)) {
        response["relationship"] = "request_sent";
    } else if (has_friend(requests["received"], target_account_id)) {
        response["relationship"] = "request_received";
    } else if (account_id == target_account_id) {
        response["relationship"] = "self";
        response["friends"] = convert_friend_entries_for_client(friends);
        fill_presence_fields(response, target_account_id, false, language);
    } else {
        response["relationship"] = "none";
    }

    // Include last_updated_activity and about_me from profile if available (for friends, this is used to trigger updates on the client when profile changes)
    json profile = load_profile(target_online_id);
    response["about_me"] = profile.value("about_me", "");
    if (profile.contains("last_updated_activity")) {
        response["last_updated_activity"] = profile["last_updated_activity"];
    } else {
        profile["last_updated_activity"] = 0;
        response["last_updated_activity"] = 0;
        save_profile(target_online_id, profile); // Ensure profile file exists for future updates
    }

    log("Friend profile requested by " + online_id + " for " + target_online_id + " -> " + response["relationship"].get<std::string>());
    res.set_content(response.dump(), "application/json");
}

void handle_friend_poll(const httplib::Request &req, httplib::Response &res) {
    std::string err;
    const auto account = get_valid_account(req, "friends poll request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const std::string since_str = req.get_param_value("since");
    int64_t since_timestamp = 0;
    if (!since_str.empty()) {
        try {
            since_timestamp = std::stoll(since_str);
        } catch (...) {
            log("Invalid timestamp in poll request from " + online_id);
            res.set_content("ERR:InvalidTimestamp", "text/plain");
            return;
        }
    }

    auto poll_signal = get_friend_poll_signal(account_id);
    FriendPollWaiter waiter_guard(account_id, poll_signal);

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(30);

    while (true) {
        json changes = json::object();
        changes["friend_status"] = json::array();
        bool has_changes = false;

        // Check if there are new friend events since last poll
        json events = get_friend_events_since(account_id, since_timestamp);

        if (!events.empty()) {
            json remaining_events = json::array();
            bool has_request_received = false;
            for (const auto &e : events) {
                std::string target_account_id = e.value("account_id", "");
                if (target_account_id.empty() && e.contains("online_id") && e["online_id"].is_string())
                    target_account_id = get_account_id_from_online_id(e["online_id"].get<std::string>());
                if (target_account_id.empty())
                    continue;
                const std::string target_online_id = get_online_id_from_account_id(target_account_id);
                if (target_online_id.empty())
                    continue;
                if (e.contains("type") && e["type"] == "status_changed") {
                    json status_obj;
                    status_obj["online_id"] = target_online_id;
                    status_obj["status"] = e.value("status", "");
                    changes["friend_status"].push_back(status_obj);
                    has_changes = true;
                } else if (e.contains("type") && e["type"] == "friends_request_received") {
                    if (!has_request_received) {
                        json req_obj = e;
                        if (replace_account_id_with_online_id(req_obj))
                            remaining_events.push_back(req_obj);
                        has_request_received = true;
                    }
                } else {
                    json event_obj = e;
                    if (replace_account_id_with_online_id(event_obj))
                        remaining_events.push_back(event_obj);
                }
            }
            if (!remaining_events.empty()) {
                changes["events"] = remaining_events;
                has_changes = true;
            }
        }

        if (has_changes) {
            std::string details = "Poll: " + online_id + " - ";
            std::vector<std::string> update_types;

            if (changes.contains("events"))
                update_types.push_back(std::to_string(changes["events"].size()) + " event(s)");
            if (!changes["friend_status"].empty())
                update_types.push_back(std::to_string(changes["friend_status"].size()) + " status change(s)");

            for (size_t i = 0; i < update_types.size(); ++i) {
                details += update_types[i];
                if (i < update_types.size() - 1)
                    details += ", ";
            }

            log(details);
            res.set_content(changes.dump(), "application/json");
            return;
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto remaining = timeout - elapsed;

        if (remaining <= std::chrono::seconds(0)) {
            json empty = json::object();
            res.set_content(empty.dump(), "application/json");
            return;
        }

        std::unique_lock<std::mutex> lock(friends_cv_mutex);
        poll_signal->cv.wait_for(lock, remaining);
        // When woken up, loop back immediately and check status (FAST: no I/O needed)
    }
}

void handle_friend_presence(const httplib::Request &req, httplib::Response &res) {
    std::string err;
    const auto account = get_valid_account(req, "friends presence", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const std::string status = req.get_param_value("status");
    if (status.empty()) {
        log("Missing status parameter on presence update for online ID " + online_id);
        res.set_content("ERR:MissingStatus", "text/plain");
        return;
    }

    const std::string now_playing = req.get_param_value("now_playing");

    std::string old_status = "offline";
    bool old_online = false;
    bool status_changed = false;
    bool now_playing_changed = false;
    bool should_push_online = false;
    bool should_push_offline = false;

    {
        std::lock_guard<std::mutex> lock(online_users_mutex);
        auto status_it = presence_status.find(account_id);
        if (status_it != presence_status.end())
            old_status = status_it->second;
        old_online = (old_status != "offline");
        const bool had_pending_online_poll = pending_online_poll.contains(account_id);
        std::string old_now_playing = "";
        auto now_playing_it = online_now_playing.find(account_id);
        if (now_playing_it != online_now_playing.end())
            old_now_playing = now_playing_it->second;

        if (status == "online" || status == "not_available") {
            // Update timestamp in memory (heartbeat)
            online_users[account_id] = std::time(0);
            online_now_playing[account_id] = now_playing;
            presence_status[account_id] = status;
            status_changed = (old_status != status);
            now_playing_changed = old_online && (old_now_playing != now_playing);

            if (status == "not_available") {
                if (old_status == "offline")
                    pending_online_poll.insert(account_id);
                else
                    pending_online_poll.erase(account_id);
            } else {
                should_push_online = status_changed && ((old_status == "offline") || had_pending_online_poll);
                pending_online_poll.erase(account_id);
            }

            // Mark status change timestamp
            if (status_changed || now_playing_changed) {
                last_status_change[account_id] = std::time(0);
            }

            // Wake up monitor if this is first online user
            if (online_users.size() == 1) {
                online_monitor_cv.notify_one();
            }
        } else if (status == "offline") {
            // Remove from map = offline
            online_users.erase(account_id);
            online_now_playing.erase(account_id);
            presence_status.erase(account_id);
            pending_online_poll.erase(account_id);
            status_changed = (old_status != "offline");
            now_playing_changed = false;
            should_push_offline = status_changed;

            // Mark status change timestamp
            if (status_changed) {
                last_status_change[account_id] = std::time(0);
            }
        } else {
            res.set_content("ERR:InvalidStatus", "text/plain");
            return;
        }
    }

    // Notify polls if status changed
    if (status_changed) {
        log("Status changed for: " + online_id + " -> " + status);
        if (should_push_online)
            push_status_event_to_friends(online_id, true);
        else if (should_push_offline)
            push_status_event_to_friends(online_id, false);
    } else if (now_playing_changed) {
        const std::string now_playing_name = get_stitle_name(now_playing, "");
        log("Now playing updated for: " + online_id + " -> " + now_playing + " (" + now_playing_name + ")");
    }

    res.set_content("OK", "text/plain");
}

void handle_friend_search(const httplib::Request &req, httplib::Response &res) {
    std::string err;
    const auto account = get_valid_account(req, "friends search", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    std::string query = trim_online_id(req.get_param_value("query"));
    if (query.size() < 3) {
        res.set_content("ERR:QueryTooShort", "text/plain");
        return;
    }

    // Case-insensitive search
    query = lowercase_online_id(query);

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].is_object()) {
        log("Invalid users database format");
        res.set_content("ERR:InternalError", "text/plain");
        return;
    }

    json results = json::array();
    for (const auto &[user_account_id, user] : db["users"].items()) {
        if (user_account_id == account_id)
            continue;

        if (!user.contains("online_id") || !user["online_id"].is_string() || user["online_id"].empty())
            continue;

        const std::string user_online_id = user["online_id"];
        const std::string lower_online_id = lowercase_online_id(user_online_id);

        if (lower_online_id.find(query) != std::string::npos) {
            json entry;
            entry["online_id"] = user_online_id;
            results.push_back(entry);
        }
    }

    log("Friend search by " + online_id + " for '" + query + "' -> " + std::to_string(results.size()) + " result(s)");
    res.set_content(results.dump(), "application/json");
}
