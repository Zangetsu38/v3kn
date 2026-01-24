// v3knr project
// Copyright (C) 2026 Vita3K team

#include "friend/friend.h"
#include "utils/utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "pugixml.hpp"

// In-memory online status: NPID -> last presence timestamp (not persisted to disk)
static std::unordered_map<std::string, int64_t> online_users;
static std::unordered_map<std::string, int64_t> last_status_change; // NPID -> timestamp of last online/offline change
static std::unordered_map<std::string, std::string> online_now_playing; // NPID -> now playing string
static std::unordered_map<std::string, std::vector<json>> friend_events; // NPID -> list of change events
static std::unordered_map<std::string, std::string> presence_status; // NPID -> online/offline/not_available
static std::unordered_set<std::string> pending_online_poll; // NPID -> waiting to poll on online
static std::mutex online_users_mutex;
static std::mutex friend_events_mutex;
static std::mutex friend_events_file_mutex;
static std::condition_variable online_monitor_cv;
static std::atomic<bool> monitor_running{ true };
static json load_friends(const std::string &npid, const std::string &group);
static json load_friends_data(const std::string &npid);

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
            friend_events[it.key()] = it.value().get<std::vector<json>>();
        }
    }
}

static void save_friend_events_to_disk() {
    std::lock_guard<std::mutex> file_lock(friend_events_file_mutex);
    json data = json::object();
    for (const auto &[npid, events] : friend_events) {
        data[npid] = events;
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

static std::shared_ptr<FriendPollSignal> get_friend_poll_signal(const std::string &npid) {
    std::lock_guard<std::mutex> lock(friend_poll_signals_mutex);
    auto &signal = friend_poll_signals[npid];
    if (!signal) {
        signal = std::make_shared<FriendPollSignal>();
    }
    return signal;
}

static void push_friend_status_event(const std::string &npid, const std::string &target_npid, bool is_online) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    json event;
    event["type"] = "status_changed";
    event["npid"] = target_npid;
    event["status"] = is_online ? "online" : "offline";
    event["at"] = std::time(0);
    friend_events[npid].push_back(event);
}

static void notify_friend_poll(const std::string &npid) {
    std::shared_ptr<FriendPollSignal> signal;
    {
        std::lock_guard<std::mutex> lock(friend_poll_signals_mutex);
        auto it = friend_poll_signals.find(npid);
        if (it != friend_poll_signals.end()) {
            signal = it->second;
        }
    }
    if (signal) {
        signal->cv.notify_one();
    }
}

static void push_status_event_to_friends(const std::string &npid, bool is_online) {
    json user_friends = load_friends(npid, "friends");
    std::lock_guard<std::mutex> lock(online_users_mutex);
    for (const auto &f : user_friends) {
        if (!f.contains("npid"))
            continue;
        const std::string friend_npid = f["npid"].get<std::string>();
        if (!online_users.contains(friend_npid))
            continue;
        push_friend_status_event(friend_npid, npid, is_online);
        notify_friend_poll(friend_npid);
    }
}

struct FriendPollWaiter {
    std::string npid;
    std::shared_ptr<FriendPollSignal> signal;

    FriendPollWaiter(const std::string &npid, std::shared_ptr<FriendPollSignal> signal)
        : npid(npid)
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
            auto it = friend_poll_signals.find(npid);
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
                const std::string npid = it->first;
                timed_out_users.push_back(npid);
                online_now_playing.erase(npid);
                presence_status.erase(npid);
                pending_online_poll.erase(npid);
                it = online_users.erase(it);
            } else {
                ++it;
            }
        }

        // Notify polls about status changes
        if (!timed_out_users.empty()) {
            const int64_t change_time = std::time(0);

            // Mark status change timestamp for timed out users
            for (const auto &npid : timed_out_users) {
                last_status_change[npid] = change_time;
            }

            lock.unlock(); // Release lock before notifications

            for (const auto &npid : timed_out_users) {
                log("User timeout detected: " + npid + " -> offline");
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
static std::string get_friends_path(const std::string &npid) {
    fs::path friends_dir = fs::path("v3kn") / "Users" / npid;
    fs::create_directories(friends_dir);
    return (friends_dir / "friends.json").string();
}

// Helper: Push a friend event for a user
static void push_friend_event(const std::string &npid, const std::string &type, const std::string &target_npid) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    json event;
    event["type"] = type;
    event["npid"] = target_npid;
    event["at"] = std::time(0);
    friend_events[npid].push_back(event);
    save_friend_events_to_disk();
}

static void remove_friend_event(const std::string &npid, const std::string &type, const std::string &target_npid) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    auto it = friend_events.find(npid);
    if (it == friend_events.end())
        return;
    auto &events = it->second;
    events.erase(
        std::remove_if(events.begin(), events.end(),
            [&type, &target_npid](const json &event) {
                return event.value("type", "") == type && event.value("npid", "") == target_npid;
            }),
        events.end());
    if (events.empty()) {
        friend_events.erase(it);
    }
    save_friend_events_to_disk();
}

// Helper: Get friend events since a timestamp for a user
static json get_friend_events_since(const std::string &npid, int64_t since) {
    std::lock_guard<std::mutex> lock(friend_events_mutex);
    json result = json::array();
    auto it = friend_events.find(npid);
    if (it != friend_events.end()) {
        result = std::move(it->second);
        friend_events.erase(it);
        save_friend_events_to_disk();
    }
    return result;
}

// Helper: Load friends data from file
static json load_friends(const std::string &npid, const std::string &group) {
    std::string path = get_friends_path(npid);
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

static json load_friends_data(const std::string &npid) {
    json friends_data;
    friends_data["friends"] = load_friends(npid, "friends");
    friends_data["friend_requests"] = load_friends(npid, "friend_requests");
    friends_data["players_blocked"] = load_friends(npid, "players_blocked");
    return friends_data;
}

// Helper: Save friends data to file
static void save_friends(const std::string &npid, const json &friends_data) {
    std::string path = get_friends_path(npid);
    std::ofstream f(path);
    f << friends_data.dump(2);
}

// Helper: Find friend by NPID in array of objects
static bool has_friend(const json &friends, const std::string &npid) {
    for (const auto &f : friends) {
        if (f.is_object() && f.contains("npid") && f["npid"] == npid)
            return true;
    }
    return false;
}

// Helper: Remove friend by NPID from array of objects
static void remove_friend(json &friends, const std::string &npid) {
    for (auto it = friends.begin(); it != friends.end(); ++it) {
        if (it->is_object() && it->contains("npid") && (*it)["npid"] == npid) {
            friends.erase(it);
            return;
        }
    }
}

// Helper: Check if user is online (in the online_users map)
static bool is_user_online(const std::string &npid) {
    std::lock_guard<std::mutex> lock(online_users_mutex);
    return online_users.contains(npid);
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

static json load_trophies_summary(const std::string &npid) {
    json summary = json::object();
    summary["level"] = 1;
    summary["progress"] = 0;
    summary["total"] = 0;
    summary["bronze"] = 0;
    summary["silver"] = 0;
    summary["gold"] = 0;
    summary["platinum"] = 0;

    const fs::path trophies_path = fs::path("v3kn") / "Users" / npid / "trophy" / "trophies.xml";
    pugi::xml_document doc;
    if (!doc.load_file(trophies_path.string().c_str()))
        return summary;

    const auto root = doc.child("trophies");
    if (!root)
        return summary;

    int64_t unlocked_count = 0;
    int64_t bronze = 0;
    int64_t silver = 0;
    int64_t gold = 0;
    int64_t platinum = 0;

    for (const auto &trophy : root.children("trophy")) {
        unlocked_count += trophy.attribute("unlocked_count").as_int();
        bronze += trophy.attribute("bronze").as_int();
        silver += trophy.attribute("silver").as_int();
        gold += trophy.attribute("gold").as_int();
        platinum += trophy.attribute("platinum").as_int();
    }

    const int64_t total = unlocked_count > 0 ? unlocked_count : bronze + silver + gold + platinum;
    const int64_t points = (bronze * 15) + (silver * 30) + (gold * 90) + (platinum * 300);

    int level = 1;
    int progress = 0;
    calculate_trophy_level(points, level, progress);

    summary["level"] = level;
    summary["progress"] = progress;
    summary["total"] = total;
    summary["bronze"] = bronze;
    summary["silver"] = silver;
    summary["gold"] = gold;
    summary["platinum"] = platinum;
    return summary;
}

static void fill_presence_fields(json &status_obj, const std::string &npid, bool include_last_activity) {
    std::lock_guard<std::mutex> lock(online_users_mutex);
    const auto status_it = presence_status.find(npid);
    const std::string status = (status_it != presence_status.end()) ? status_it->second : "offline";
    const bool is_online = status != "offline";
    std::string now_playing = "";
    if (is_online) {
        auto now_playing_it = online_now_playing.find(npid);
        if (now_playing_it != online_now_playing.end())
            now_playing = now_playing_it->second;
    }
    status_obj["status"] = status;
    status_obj["now_playing"] = now_playing;
    if (include_last_activity) {
        status_obj["last_activity"] = last_status_change.contains(npid) ? last_status_change[npid] : 0;
    }
}

void handle_friend_add(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friend add request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend add request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    if (npid == target_npid) {
        log("Cannot add yourself as friend: " + npid);
        res.set_content("ERR:CannotAddYourself", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend add request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(npid);
    json target_friends = load_friends_data(target_npid);

    const bool is_blocked_by_target = has_friend(target_friends["players_blocked"], npid);

    if (has_friend(user_friends["friends"], target_npid)) {
        log("Already friends: " + npid + " and " + target_npid);
        res.set_content("ERR:AlreadyFriends", "text/plain");
        return;
    }

    if (has_friend(user_friends["friend_requests"]["sent"], target_npid)) {
        log("Friend request already sent from " + npid + " to " + target_npid);
        res.set_content("ERR:RequestAlreadySent", "text/plain");
        return;
    }

    if (is_blocked_by_target) {
        if (!has_friend(user_friends["friend_requests"]["sent"], target_npid)) {
            json req_obj;
            req_obj["npid"] = target_npid;
            req_obj["sent_at"] = std::time(0);
            user_friends["friend_requests"]["sent"].push_back(req_obj);
            save_friends(npid, user_friends);
        }

        log("Friend request silently stored from " + npid + " to blocked target " + target_npid);
        res.set_content("OK:RequestSent", "text/plain");
        return;
    }

    const bool has_received_request = has_friend(user_friends["friend_requests"]["received"], target_npid);
    const bool has_target_sent_request = has_friend(target_friends["friend_requests"]["sent"], npid);
    if (has_received_request || has_target_sent_request) {
        // Auto-accept
        if (has_received_request) {
            remove_friend(user_friends["friend_requests"]["received"], target_npid);
        }
        if (has_target_sent_request) {
            remove_friend(target_friends["friend_requests"]["sent"], npid);
        }

        json friend_obj;
        friend_obj["npid"] = target_npid;
        friend_obj["since"] = std::time(0);
        user_friends["friends"].push_back(friend_obj);

        json friend_obj2;
        friend_obj2["npid"] = npid;
        friend_obj2["since"] = std::time(0);
        target_friends["friends"].push_back(friend_obj2);

        save_friends(npid, user_friends);
        save_friends(target_npid, target_friends);

        log("Auto-accepted friend request: " + npid + " <-> " + target_npid);
        res.set_content("OK:FriendAdded", "text/plain");
        return;
    }

    // Send friend request
    json req_obj;
    req_obj["npid"] = target_npid;
    req_obj["sent_at"] = std::time(0);
    user_friends["friend_requests"]["sent"].push_back(req_obj);

    json req_obj2;
    req_obj2["npid"] = npid;
    req_obj2["received_at"] = std::time(0);
    target_friends["friend_requests"]["received"].push_back(req_obj2);

    save_friends(npid, user_friends);
    save_friends(target_npid, target_friends);

    push_friend_event(target_npid, "friends_request_received", npid);

    log("Friend request sent from " + npid + " to " + target_npid);

    notify_friend_poll(target_npid);

    res.set_content("OK:RequestSent", "text/plain");
}

void handle_friend_accept(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friend accept request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend accept request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend accept request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(npid);
    json target_friends = load_friends_data(target_npid);

    if (!has_friend(user_friends["friend_requests"]["received"], target_npid)) {
        log("No friend request from " + target_npid + " to accept by " + npid);
        res.set_content("ERR:NoRequestFound", "text/plain");
        return;
    }

    // Accept request
    remove_friend(user_friends["friend_requests"]["received"], target_npid);
    remove_friend(target_friends["friend_requests"]["sent"], npid);

    json friend_obj;
    friend_obj["npid"] = target_npid;
    friend_obj["since"] = std::time(0);
    user_friends["friends"].push_back(friend_obj);

    json friend_obj2;
    friend_obj2["npid"] = npid;
    friend_obj2["since"] = std::time(0);
    target_friends["friends"].push_back(friend_obj2);

    save_friends(npid, user_friends);
    save_friends(target_npid, target_friends);

    log("Friend request accepted: " + npid + " <-> " + target_npid);

    res.set_content("OK:FriendAdded", "text/plain");
}

void handle_friend_reject(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friend reject request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend reject request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend reject request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(npid);
    json target_friends = load_friends_data(target_npid);

    if (!has_friend(user_friends["friend_requests"]["received"], target_npid)) {
        log("No friend request from " + target_npid + " to reject by " + npid);
        res.set_content("ERR:NoRequestFound", "text/plain");
        return;
    }

    // Reject request
    remove_friend(user_friends["friend_requests"]["received"], target_npid);
    remove_friend(target_friends["friend_requests"]["sent"], npid);

    save_friends(npid, user_friends);
    save_friends(target_npid, target_friends);

    log("Friend request rejected: " + target_npid + " -> " + npid);

    res.set_content("OK:RequestRejected", "text/plain");
}

void handle_friend_remove(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friend remove request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend remove request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend remove request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(npid);
    json target_friends = load_friends_data(target_npid);

    if (!has_friend(user_friends["friends"], target_npid)) {
        log("Not friends: " + npid + " and " + target_npid);
        res.set_content("ERR:NotFriends", "text/plain");
        return;
    }

    // Remove friendship
    remove_friend(user_friends["friends"], target_npid);
    remove_friend(target_friends["friends"], npid);

    save_friends(npid, user_friends);
    save_friends(target_npid, target_friends);

    log("Friendship removed: " + npid + " <-> " + target_npid);

    res.set_content("OK:FriendRemoved", "text/plain");
}

void handle_friend_cancel(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friend cancel request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend cancel request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend cancel request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(npid);
    json target_friends = load_friends_data(target_npid);

    if (!has_friend(user_friends["friend_requests"]["sent"], target_npid)) {
        log("No friend request to cancel from " + npid + " to " + target_npid);
        res.set_content("ERR:NoRequestFound", "text/plain");
        return;
    }

    // Cancel the friend request
    remove_friend(user_friends["friend_requests"]["sent"], target_npid);
    remove_friend(target_friends["friend_requests"]["received"], npid);

    save_friends(npid, user_friends);
    save_friends(target_npid, target_friends);

    remove_friend_event(target_npid, "friends_request_received", npid);

    log("Friend request cancelled: " + npid + " -> " + target_npid);

    res.set_content("OK:RequestCancelled", "text/plain");
}

void handle_friend_block(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friend block request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend block request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    if (npid == target_npid) {
        log("Cannot block yourself: " + npid);
        res.set_content("ERR:CannotBlockYourself", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend block request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(npid);
    json target_friends = load_friends_data(target_npid);

    if (!has_friend(user_friends["players_blocked"], target_npid)) {
        json block_obj;
        block_obj["npid"] = target_npid;
        block_obj["blocked_at"] = std::time(0);
        user_friends["players_blocked"].push_back(block_obj);
    }

    const bool is_friends = has_friend(user_friends["friends"], target_npid);
    const bool user_sent_request = has_friend(user_friends["friend_requests"]["sent"], target_npid);
    const bool target_sent_request = has_friend(target_friends["friend_requests"]["sent"], npid);

    if (is_friends) {
        remove_friend(user_friends["friends"], target_npid);
        remove_friend(target_friends["friends"], npid);
    }

    if (user_sent_request) {
        remove_friend(user_friends["friend_requests"]["sent"], target_npid);
        remove_friend(target_friends["friend_requests"]["received"], npid);
    }

    if (target_sent_request) {
        remove_friend(user_friends["friend_requests"]["received"], target_npid);
    }

    save_friends(npid, user_friends);
    if (is_friends || user_sent_request) {
        save_friends(target_npid, target_friends);
    }

    log("Player blocked: " + npid + " -> " + target_npid);
    res.set_content("OK:PlayerBlocked", "text/plain");
}

void handle_friend_unblock(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friend unblock request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend unblock request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend unblock request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json user_friends = load_friends_data(npid);
    json target_friends = load_friends_data(target_npid);

    remove_friend(user_friends["players_blocked"], target_npid);

    const bool target_sent_request = has_friend(target_friends["friend_requests"]["sent"], npid);
    if (target_sent_request && !has_friend(user_friends["friend_requests"]["received"], target_npid)) {
        json req_obj;
        req_obj["npid"] = target_npid;
        req_obj["received_at"] = std::time(0);
        user_friends["friend_requests"]["received"].push_back(req_obj);
        notify_friend_poll(npid);
    }

    save_friends(npid, user_friends);

    log("Player unblocked: " + npid + " -> " + target_npid);
    res.set_content("OK:PlayerUnblocked", "text/plain");
}

void handle_friend_list(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friends list request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string group = req.get_param_value("group");
    if (group.empty()) {
        res.set_content("ERR:MissingGroup", "text/plain");
        return;
    }

    json response = json::object();
    if (group == "friends") {
        json friends = load_friends(npid, "friends");
        json enriched_friends = json::array();
        for (const auto &f : friends) {
            if (!f.contains("npid"))
                continue;
            json entry = f;
            const std::string friend_npid = f["npid"].get<std::string>();
            fill_presence_fields(entry, friend_npid, false);
            entry["trophy_level"] = load_trophies_summary(friend_npid)["level"];
            enriched_friends.push_back(entry);
        }
        response["friends"] = enriched_friends;
        json self_entry = json::object();
        self_entry["npid"] = npid;
        self_entry["since"] = 0;
        fill_presence_fields(self_entry, npid, false);
        self_entry["trophy_level"] = load_trophies_summary(npid)["level"];
        response["self"] = self_entry;
    } else if (group == "friend_requests") {
        response["friend_requests"] = load_friends(npid, "friend_requests");
    } else if (group == "players_blocked") {
        response["players_blocked"] = load_friends(npid, "players_blocked");
    } else {
        res.set_content("ERR:InvalidGroup", "text/plain");
        return;
    }

    log("Friends list requested by " + npid + " (" + group + ")");
    res.set_content(response.dump(), "application/json");
}

void handle_friend_profile(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "friends profile request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("Missing target_npid on friend profile request for NPID " + npid);
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    if (!db["users"].contains(target_npid)) {
        log("Friend profile request to non-existing NPID " + target_npid + " by " + npid);
        res.set_content("ERR:UserNotFound", "text/plain");
        return;
    }

    json response = json::object();
    response["npid"] = target_npid;
    response["friends"] = json::array();
    response["trophies"] = load_trophies_summary(target_npid);

    json friends = load_friends(npid, "friends");
    json requests = load_friends(npid, "friend_requests");
    json blocked = load_friends(npid, "players_blocked");

    if (has_friend(blocked, target_npid)) {
        response["relationship"] = "blocked";
    } else if (has_friend(friends, target_npid)) {
        response["relationship"] = "friends";
        response["friends"] = load_friends(target_npid, "friends");
        fill_presence_fields(response, target_npid, false);
    } else if (has_friend(requests["sent"], target_npid)) {
        response["relationship"] = "request_sent";
    } else if (has_friend(requests["received"], target_npid)) {
        response["relationship"] = "request_received";
    } else if (npid == target_npid) {
        response["relationship"] = "self";
        response["friends"] = friends;
        fill_presence_fields(response, target_npid, false);
    } else {
        response["relationship"] = "none";
    }

    log("Friend profile requested by " + npid + " for " + target_npid + " -> " + response["relationship"].get<std::string>());
    res.set_content(response.dump(), "application/json");
}

void handle_friend_poll(const httplib::Request &req, httplib::Response &res) {
    std::string err;
    const std::string npid = get_valid_npid(req, "friends poll request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string since_str = req.get_param_value("since");
    int64_t since_timestamp = 0;
    if (!since_str.empty()) {
        try {
            since_timestamp = std::stoll(since_str);
        } catch (...) {
            log("Invalid timestamp in poll request from " + npid);
            res.set_content("ERR:InvalidTimestamp", "text/plain");
            return;
        }
    }

    auto poll_signal = get_friend_poll_signal(npid);
    FriendPollWaiter waiter_guard(npid, poll_signal);

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(30);

    while (true) {
        json changes = json::object();
        changes["friend_status"] = json::array();
        bool has_changes = false;

        // Check if there are new friend events since last poll
        json events = get_friend_events_since(npid, since_timestamp);

        if (!events.empty()) {
            json remaining_events = json::array();
            bool has_request_received = false;
            for (const auto &e : events) {
                if (e.contains("type") && e["type"] == "status_changed") {
                    json status_obj;
                    status_obj["npid"] = e.value("npid", "");
                    status_obj["status"] = e.value("status", "");
                    changes["friend_status"].push_back(status_obj);
                    has_changes = true;
                } else if (e.contains("type") && e["type"] == "friends_request_received") {
                    if (!has_request_received) {
                        remaining_events.push_back(e);
                        has_request_received = true;
                    }
                } else {
                    remaining_events.push_back(e);
                }
            }
            if (!remaining_events.empty()) {
                changes["events"] = remaining_events;
                has_changes = true;
            }
        }

        if (has_changes) {
            std::string details = "Poll: " + npid + " - ";
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
    const std::string npid = get_valid_npid(req, "friends presence", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string status = req.get_param_value("status");
    if (status.empty()) {
        log("Missing status parameter on presence update for NPID " + npid);
        res.set_content("ERR:MissingStatus", "text/plain");
        return;
    }

    const std::string now_playing = req.get_param_value("now_playing");

    std::string old_status = "offline";
    bool old_online = false;
    bool status_changed = false;
    bool now_playing_changed = false;

    {
        std::lock_guard<std::mutex> lock(online_users_mutex);
        auto status_it = presence_status.find(npid);
        if (status_it != presence_status.end())
            old_status = status_it->second;
        old_online = (old_status != "offline");
        std::string old_now_playing = "";
        auto now_playing_it = online_now_playing.find(npid);
        if (now_playing_it != online_now_playing.end())
            old_now_playing = now_playing_it->second;

        if (status == "online" || status == "not_available") {
            // Update timestamp in memory (heartbeat)
            online_users[npid] = std::time(0);
            online_now_playing[npid] = now_playing;
            presence_status[npid] = status;
            status_changed = (old_status != status);
            now_playing_changed = old_online && (old_now_playing != now_playing);

            if (status == "not_available") {
                if (old_status == "offline")
                    pending_online_poll.insert(npid);
                else
                    pending_online_poll.erase(npid);
            }

            // Mark status change timestamp
            if (status_changed || now_playing_changed) {
                last_status_change[npid] = std::time(0);
            }

            // Wake up monitor if this is first online user
            if (online_users.size() == 1) {
                online_monitor_cv.notify_one();
            }
        } else if (status == "offline") {
            // Remove from map = offline
            online_users.erase(npid);
            online_now_playing.erase(npid);
            presence_status.erase(npid);
            pending_online_poll.erase(npid);
            status_changed = (old_status != "offline");
            now_playing_changed = false;

            // Mark status change timestamp
            if (status_changed) {
                last_status_change[npid] = std::time(0);
            }
        } else {
            res.set_content("ERR:InvalidStatus", "text/plain");
            return;
        }
    }

    // Notify polls if status changed
    if (status_changed) {
        log("Status changed for: " + npid + " -> " + status);
        if (status == "online") {
            const bool should_poll = (old_status == "offline") || pending_online_poll.contains(npid);
            if (should_poll) {
                push_status_event_to_friends(npid, true);
            }
            pending_online_poll.erase(npid);
        }
    } else if (now_playing_changed) {
        log("Now playing updated for: " + npid + " -> " + now_playing);
    }

    res.set_content("OK", "text/plain");
}

void handle_friend_search(const httplib::Request &req, httplib::Response &res) {
    std::string err;
    const std::string npid = get_valid_npid(req, "friends search", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    std::string query = req.get_param_value("query");
    if (query.size() < 3) {
        res.set_content("ERR:QueryTooShort", "text/plain");
        return;
    }

    // Case-insensitive search
    std::transform(query.begin(), query.end(), query.begin(), ::tolower);

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    json results = json::array();
    for (auto it = db["users"].begin(); it != db["users"].end(); ++it) {
        const std::string &user_npid = it.key();
        if (user_npid == npid)
            continue;

        std::string lower_npid = user_npid;
        std::transform(lower_npid.begin(), lower_npid.end(), lower_npid.begin(), ::tolower);

        if (lower_npid.find(query) != std::string::npos) {
            json entry;
            entry["npid"] = user_npid;
            results.push_back(entry);
        }
    }

    log("Friend search by " + npid + " for '" + query + "' -> " + std::to_string(results.size()) + " result(s)");
    res.set_content(results.dump(), "application/json");
}

void notify_avatar_changed(const std::string &npid) {
    log("Avatar changed for " + npid);
}
