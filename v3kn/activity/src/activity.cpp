// v3knr project
// Copyright (C) 2026 Vita3K team

#include <account/account.h>
#include <activity/activity.h>
#include <friend/friend.h>
#include <utils/utils.h>

#include <ctime>
#include <fstream>
#include <unordered_map>

static std::mutex activities_mutex;

static constexpr auto ACTIVITY_NOTIFICATION_COMMENTED_ON_FRIEND_ACTIVITY = "commented_on_friend_activity";
static constexpr auto ACTIVITY_NOTIFICATION_COMMENTED_ON_YOUR_ACTIVITY = "commented_on_your_activity";
static constexpr auto ACTIVITY_NOTIFICATION_COMMENTED_AND_LIKES_YOUR_ACTIVITY = "commented_and_likes_your_activity";
static constexpr auto ACTIVITY_NOTIFICATION_LIKES_YOUR_ACTIVITY = "likes_your_activity";

static int64_t migrate_second_timestamp_to_millis(std::unordered_map<int64_t, int64_t> &per_second_offsets, int64_t timestamp_seconds) {
    const int64_t offset = per_second_offsets[timestamp_seconds]++;
    return (timestamp_seconds * 1000) + offset;
}

void migrate_activities_created_at_to_milliseconds() {
    std::lock_guard<std::mutex> lock(account_mutex);
    json db = load_users();
    if (!db.contains("users") || !db["users"].is_object())
        return;

    for (const auto &[account_id, user] : db["users"].items()) {
        if (!user.contains("online_id") || !user["online_id"].is_string() || user["online_id"].empty())
            continue;

        const std::string online_id = user["online_id"].get<std::string>();
        const fs::path activities_path{ fs::path("v3kn") / "Users" / online_id / "activities.json" };
        if (!fs::exists(activities_path))
            continue;

        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        json activities;
        {
            std::ifstream f(activities_path);
            f >> activities;
        }

        if (!activities.contains("activities") || !activities["activities"].is_array())
            continue;

        bool modified = false;
        std::unordered_map<int64_t, int64_t> activity_offsets;
        std::unordered_map<int64_t, int64_t> comment_offsets;

        for (auto &activity : activities["activities"]) {
            if (activity.contains("created_at") && activity["created_at"].is_number_integer()) {
                const int64_t created_at = activity["created_at"].get<int64_t>();
                if (created_at > 0 && created_at < 1000000000000LL) {
                    activity["created_at"] = migrate_second_timestamp_to_millis(activity_offsets, created_at);
                    modified = true;
                }
            }

            if (!activity.contains("comments") || !activity["comments"].is_array())
                continue;

            for (auto &comment : activity["comments"]) {
                if (!comment.is_object() || !comment.contains("created_at") || !comment["created_at"].is_number_integer())
                    continue;

                const int64_t created_at = comment["created_at"].get<int64_t>();
                if (created_at > 0 && created_at < 1000000000000LL) {
                    comment["created_at"] = migrate_second_timestamp_to_millis(comment_offsets, created_at);
                    modified = true;
                }
            }
        }

        if (!modified)
            continue;

        std::ofstream out(activities_path);
        out << activities.dump(2);
        log("Migrated activity created_at timestamps to milliseconds for online ID " + online_id + " (account ID " + account_id + ")");
    }
}

static void queue_activity_notification_event(const std::string &recipient_account_id, const std::string &group, int64_t activity_created_at, const std::string &activity_owner_account_id = "") {
    bool inserted = false;
    update_poll_events(recipient_account_id,
        [&](json &events) {
            json event;
            event["type"] = "activity";
            event["group"] = group;
            event["created_at"] = activity_created_at;
            if (!activity_owner_account_id.empty())
                event["account_id"] = activity_owner_account_id;

            events.push_back(event);
            inserted = true;
        });

    if (inserted)
        notify_friend_poll_for_account(recipient_account_id);
}

static void queue_activity_owner_notification(const std::string &recipient_account_id, const std::string &actor_account_id, int64_t activity_created_at, bool is_comment) {
    if (recipient_account_id.empty() || actor_account_id.empty() || recipient_account_id == actor_account_id)
        return;

    bool changed = false;
    bool has_existing = false;

    update_poll_events(recipient_account_id,
        [&](json &events) {
            for (auto &event : events) {
                if (!event.is_object() || event.value("type", "") != "activity")
                    continue;
                if (event.value("created_at", int64_t{ 0 }) != activity_created_at)
                    continue;

                const std::string group = event.value("group", "");
                if (group != ACTIVITY_NOTIFICATION_COMMENTED_ON_YOUR_ACTIVITY && group != ACTIVITY_NOTIFICATION_COMMENTED_AND_LIKES_YOUR_ACTIVITY && group != ACTIVITY_NOTIFICATION_LIKES_YOUR_ACTIVITY)
                    continue;

                has_existing = true;
                if (is_comment && group == ACTIVITY_NOTIFICATION_LIKES_YOUR_ACTIVITY) {
                    event["group"] = ACTIVITY_NOTIFICATION_COMMENTED_AND_LIKES_YOUR_ACTIVITY;
                    changed = true;
                } else if (!is_comment && group == ACTIVITY_NOTIFICATION_COMMENTED_ON_YOUR_ACTIVITY) {
                    event["group"] = ACTIVITY_NOTIFICATION_COMMENTED_AND_LIKES_YOUR_ACTIVITY;
                    changed = true;
                }
                return;
            }
        });

    if (changed || has_existing) {
        if (changed)
            notify_friend_poll_for_account(recipient_account_id);
        return;
    }

    queue_activity_notification_event(recipient_account_id,
        is_comment ? ACTIVITY_NOTIFICATION_COMMENTED_ON_YOUR_ACTIVITY : ACTIVITY_NOTIFICATION_LIKES_YOUR_ACTIVITY,
        activity_created_at);
}

static void queue_friend_activity_comment_notifications(const std::string &activity_owner_account_id, const std::string &activity_owner_online_id, const std::string &actor_account_id, int64_t activity_created_at) {
    if (activity_owner_account_id.empty() || activity_owner_online_id.empty())
        return;

    const json friends = load_friends(activity_owner_online_id, "friends");
    if (!friends.is_array())
        return;

    for (const auto &friend_entry : friends) {
        if (!friend_entry.is_object() || !friend_entry.contains("account_id") || !friend_entry["account_id"].is_string())
            continue;

        const std::string recipient_account_id = friend_entry["account_id"].get<std::string>();
        if (recipient_account_id.empty() || recipient_account_id == activity_owner_account_id || recipient_account_id == actor_account_id)
            continue;

        bool exists = false;
        update_poll_events(recipient_account_id,
            [&](json &events) {
                for (const auto &event : events) {
                    if (!event.is_object() || event.value("type", "") != "activity")
                        continue;
                    if (event.value("group", "") != ACTIVITY_NOTIFICATION_COMMENTED_ON_FRIEND_ACTIVITY)
                        continue;
                    if (event.value("created_at", int64_t{ 0 }) != activity_created_at)
                        continue;
                    if (event.value("account_id", "") != activity_owner_account_id)
                        continue;
                    exists = true;
                    return;
                }
            });

        if (exists)
            continue;

        queue_activity_notification_event(recipient_account_id,
            ACTIVITY_NOTIFICATION_COMMENTED_ON_FRIEND_ACTIVITY,
            activity_created_at,
            activity_owner_account_id);
    }
}

static void append_activity(const std::string &online_id, const json &activity) {
    const fs::path activities_path{ fs::path("v3kn") / "Users" / online_id / "activities.json" };
    {
        json activities;
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        if (fs::exists(activities_path)) {
            std::ifstream f(activities_path);
            f >> activities;
        } else {
            activities["activities"] = nlohmann::json::array();
        }

        // Create new activity with empty likes and comments arrays
        json entry = activity;
        entry["likes"] = nlohmann::json::array();
        entry["comments"] = nlohmann::json::array();

        // Add to list
        activities["activities"].insert(activities["activities"].begin(), std::move(entry));

        // Keep only latest 30 activities
        if (activities["activities"].size() > 30) {
            const auto diff = activities["activities"].size() - 30;
            activities["activities"].erase(activities["activities"].end() - diff, activities["activities"].end());
        }

        // Save file
        std::ofstream f(activities_path);
        f << activities.dump(2);
    }

    // Update profile timestamp
    update_profile_timestamp(online_id);

    log("Activity of type " + activity["type"].get<std::string>() + ", posted by online_id " + online_id);
}

void create_friendship_established_activity(const std::string &account_id1, const std::string &account_id2) {
    const std::string online_id1 = get_online_id_from_account_id(account_id1);
    const std::string online_id2 = get_online_id_from_account_id(account_id2);

    if (online_id1.empty()) {
        log("Failed to create friendship_established activity: missing online ID for account ID " + account_id1);
        return;
    }

    if (online_id2.empty()) {
        log("Failed to create friendship_established activity: missing online ID for account ID " + account_id2);
        return;
    }

    // Create activity for account_id1
    {
        json payload;
        payload["type"] = "friendship_established";
        payload["account_id"] = account_id1;
        payload["friend_account_id"] = account_id2;
        payload["created_at"] = get_current_time_ms();
        append_activity(online_id1, payload);
    }

    // Create activity for account_id2
    {
        json payload;
        payload["type"] = "friendship_established";
        payload["account_id"] = account_id2;
        payload["friend_account_id"] = account_id1;
        payload["created_at"] = get_current_time_ms();
        append_activity(online_id2, payload);
    }
}

void register_activity_endpoints(httplib::Server &server) {
    server.Post("/v3kn/activity/post", handle_post_activity);
    server.Post("/v3kn/activity/like", handle_like_activity);
    server.Post("/v3kn/activity/unlike", handle_unlike_activity);
    server.Post("/v3kn/activity/comment", handle_comment_activity);
    server.Post("/v3kn/activity/uncomment", handle_uncomment_activity);
    server.Post("/v3kn/activity/delete", handle_delete_activity);

    server.Get("/v3kn/activity/get", handle_get_activities);
}

void handle_post_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "post activity", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    // Parse JSON
    json payload;
    try {
        payload = json::parse(req.body);
    } catch (...) {
        log("online ID " + online_id + " try to post activity with invalid JSON");
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    payload.erase("online_id");
    payload["account_id"] = account_id;

    // Post activity
    append_activity(online_id, payload);

    // Respond
    res.set_content("OK:ActivityPosted", "text/plain");
}

void handle_like_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "like activity", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "like activity target", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_online_id = target_account->online_id;
    const std::string &target_account_id = target_account->account_id;

    const auto created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("online_id " + online_id + " try to like activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    const int64_t created_at_time = std::stoll(created_at);

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_online_id / "activities.json" };
    json activities;
    {
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        if (!fs::exists(activities_path)) {
            log("online ID " + online_id + " try to like activity for online ID " + target_online_id + " but no activities found");
            res.set_content("ERR:NoActivities", "text/plain");
            return;
        } else {
            std::ifstream f(activities_path);
            f >> activities;
        }

        // Find activity by created_at timestamp
        auto it = std::find_if(activities["activities"].begin(), activities["activities"].end(),
            [&created_at_time](const json &activity) {
                return activity["created_at"] == created_at_time;
            });
        if (it == activities["activities"].end()) {
            log("online ID " + online_id + " try to like activity for online ID " + target_online_id + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        auto &likes = (*it)["likes"];

        // Check if already has 100 likes
        if (likes.size() >= 100) {
            log("online_id " + online_id + " try to like activity for online_id " + target_online_id + " but already has 100 likes");
            res.set_content("ERR:MaxLikesReached", "text/plain");
            return;
        }

        // Add like if not already liked
        if (std::find(likes.begin(), likes.end(), account_id) == likes.end()) {
            likes.push_back(account_id);
        } else {
            log("online ID " + online_id + " try to like activity for online ID " + target_online_id + " but already liked");
            res.set_content("ERR:AlreadyLiked", "text/plain");
            return;
        }

        // Save updated activities.json
        std::ofstream f(activities_path);
        f << activities.dump(2);
    }

    // Update profile timestamp
    update_profile_timestamp(target_online_id);

    queue_activity_owner_notification(target_account_id, account_id, created_at_time, false);

    log("online ID " + online_id + " liked activity for online ID " + target_online_id);
    res.set_content("OK:Liked", "text/plain");
}

void handle_unlike_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "unlike activity", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    const auto target_account = get_valid_target_account(req, "unlike activity target", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    const std::string created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("online ID " + online_id + " try to unlike activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    const int64_t created_at_time = std::stoll(created_at);

    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_online_id / "activities.json" };
    json activities;

    if (!fs::exists(activities_path)) {
        log("online ID " + online_id + " try to unlike activity for online ID " + target_online_id + " but no activities found");
        res.set_content("ERR:NoActivities", "text/plain");
        return;
    }

    {
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        std::ifstream f(activities_path);
        f >> activities;

        auto it = std::find_if(activities["activities"].begin(), activities["activities"].end(),
            [&created_at_time](const json &activity) {
                return activity["created_at"] == created_at_time;
            });

        if (it == activities["activities"].end()) {
            log("online ID " + online_id + " try to unlike activity for online ID " + target_online_id + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        auto &likes = (*it)["likes"];
        auto like_it = std::find(likes.begin(), likes.end(), account_id);

        if (like_it == likes.end()) {
            log("online ID " + online_id + " try to unlike activity for online ID " + target_online_id + " but activity not liked");
            res.set_content("ERR:NotLiked", "text/plain");
            return;
        }

        likes.erase(like_it);

        std::ofstream out(activities_path);
        out << activities.dump(2);
    }

    // Update profile timestamp
    update_profile_timestamp(target_online_id);

    log("online ID " + online_id + " unliked activity for online ID " + target_online_id);
    res.set_content("OK:Unliked", "text/plain");
}

void handle_comment_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const auto account = get_valid_account(req, "comment activity", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    json payload;
    try {
        payload = json::parse(req.body);
    } catch (...) {
        log("online ID " + online_id + " try to comment activity with invalid JSON");
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    std::string target_online_id = payload.value("target_online_id", "");
    if (target_online_id.empty()) {
        log("online_id " + online_id + " try to comment activity with missing target online_id");
        res.set_content("ERR:MissingTargetonline_id", "text/plain");
        return;
    }

    const std::string target_account_id = get_account_id_from_online_id(target_online_id);
    if (target_account_id.empty()) {
        log("online ID " + online_id + " try to comment activity for non-existing online ID " + target_online_id);
        res.set_content("ERR:TargetOnlineIDNotFound", "text/plain");
        return;
    }

    target_online_id = get_online_id_from_account_id(target_account_id);
    if (target_online_id.empty()) {
        log("online ID " + online_id + " try to comment activity for non-existing online ID " + target_online_id);
        res.set_content("ERR:TargetOnlineIDNotFound", "text/plain");
        return;
    }

    const int64_t created_at = payload.value("created_at", int64_t{ 0 });
    if (created_at == 0) {
        log("online ID " + online_id + " try to comment activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    // Get comment from JSON and check if it's empty
    const auto comment = payload.value("comment", "");
    if (comment.empty()) {
        log("online ID " + online_id + " try to comment activity with missing comment");
        res.set_content("ERR:MissingComment", "text/plain");
        return;
    }

    // Check comment length (max 140 characters)
    if (comment.size() > 140) {
        log("online ID " + online_id + " try to comment activity with comment exceeding 140 characters");
        res.set_content("ERR:CommentTooLong", "text/plain");
        return;
    }

    // Check comment newlines (max 5 lines)
    const size_t newline_count = std::count(comment.begin(), comment.end(), '\n');
    if (newline_count > 4) {
        log("online ID " + online_id + " try to comment activity with comment exceeding 5 lines");
        res.set_content("ERR:TooManyNewlines", "text/plain");
        return;
    }

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_online_id / "activities.json" };
    json activities;
    {
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        if (!fs::exists(activities_path)) {
            log("online ID " + online_id + " try to comment activity for online ID " + target_online_id + " but no activities found");
            res.set_content("ERR:NoActivities", "text/plain");
            return;
        } else {
            std::ifstream f(activities_path);
            f >> activities;
        }

        // Find activity by created_at timestamp
        auto it = std::find_if(activities["activities"].begin(), activities["activities"].end(),
            [&created_at](const json &activity) {
                return activity["created_at"] == created_at;
            });
        if (it == activities["activities"].end()) {
            log("online_id " + online_id + " try to comment activity for online_id " + target_online_id + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        // Add comment to activity
        auto &comments = (*it)["comments"];

        // Limit to 20 comments per activity
        if (comments.size() >= 20) {
            log("online ID " + online_id + " try to comment activity for online ID " + target_online_id + " but already has 20 comments");
            res.set_content("ERR:TooManyComments", "text/plain");
            return;
        }

        // Add comment
        json comment_entry;
        comment_entry["account_id"] = account_id;
        comment_entry["comment"] = comment;
        comment_entry["created_at"] = get_current_time_ms();

        comments.push_back(comment_entry);

        // Save updated activities.json
        std::ofstream f(activities_path);
        f << activities.dump(2);
    }

    // Update profile timestamp
    update_profile_timestamp(target_online_id);

    queue_activity_owner_notification(target_account_id, account_id, created_at, true);
    queue_friend_activity_comment_notifications(target_account_id, target_online_id, account_id, created_at);

    log("online ID " + online_id + " commented activity for online ID " + target_online_id);
    res.set_content("OK:CommentAdded", "text/plain");
}

void handle_uncomment_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const auto account = get_valid_account(req, "uncomment activity", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    // Get target online ID from parameters
    const auto target_account = get_valid_target_account(req, "uncomment activity target", err, online_id);
    if (!target_account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &target_account_id = target_account->account_id;
    const std::string &target_online_id = target_account->online_id;

    // Get created_at from parameters
    const std::string created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("online ID " + online_id + " try to uncomment activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    const int64_t created_at_time = std::stoll(created_at);

    // Get comment_created_at from parameters
    const std::string comment_created_at = req.get_param_value("comment_created_at");
    if (comment_created_at.empty()) {
        log("online ID " + online_id + " try to uncomment activity with missing comment_created_at");
        res.set_content("ERR:MissingCommentCreatedAt", "text/plain");
        return;
    }

    const int64_t comment_created_at_time = std::stoll(comment_created_at);

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_online_id / "activities.json" };
    if (!fs::exists(activities_path)) {
        log("online ID " + online_id + " try to uncomment activity for online ID " + target_online_id + " but no activities found");
        res.set_content("ERR:NoActivities", "text/plain");
        return;
    }

    // Find activity and comment
    json activities;
    {
        // Lock activities.json while reading and writing
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        std::ifstream f(activities_path);
        f >> activities;
        auto it = std::find_if(activities["activities"].begin(), activities["activities"].end(),
            [&created_at_time](const json &activity) {
                return activity["created_at"] == created_at_time;
            });

        // Check if activity exists
        if (it == activities["activities"].end()) {
            log("online ID " + online_id + " try to uncomment activity for online ID " + target_online_id + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        // Find comment by comment_created_at timestamp
        auto &comments = (*it)["comments"];
        auto comment_it = std::find_if(comments.begin(), comments.end(),
            [&online_id, &comment_created_at_time](const json &comment) {
                return comment["created_at"] == comment_created_at_time;
            });

        if (comment_it == comments.end()) {
            log("online ID " + online_id + " try to uncomment activity for online ID " + target_online_id + " but no matching comment found");
            res.set_content("ERR:CommentNotFound", "text/plain");
            return;
        }

        // Check if the comment author is the same as the requester or if the requester is the owner of the activity
        if (!(*comment_it).contains("account_id")) {
            log("online ID " + online_id + " try to uncomment activity for online ID " + target_online_id + " but comment has no account ID");
            res.set_content("ERR:CommentAuthorNotFound", "text/plain");
            return;
        }

        const std::string author = (*comment_it)["account_id"].get<std::string>();
        if ((author != account_id) && (account_id != target_account_id)) {
            log("online ID " + online_id + " try to delete comment for online ID " + target_online_id + " but not allowed to delete this comment");
            res.set_content("ERR:NotAllowed", "text/plain");
            return;
        }

        // Remove comment
        comments.erase(comment_it);
        std::ofstream out(activities_path);
        out << activities.dump(2);
    }

    // Update profile timestamp
    update_profile_timestamp(target_online_id);

    log("online_id " + online_id + " uncommented activity for online_id " + target_online_id);
    res.set_content("OK:Uncommented", "text/plain");
}

void handle_delete_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const auto account = get_valid_account(req, "delete activity", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    // Get created_at from parameters
    const std::string created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("online ID " + online_id + " try to delete activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    const int64_t created_at_time = std::stoll(created_at);

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / online_id / "activities.json" };
    if (!fs::exists(activities_path)) {
        log("online_id " + online_id + " try to delete activity but no activities found");
        res.set_content("ERR:NoActivities", "text/plain");
        return;
    }

    // Find activity by created_at timestamp and delete it
    json activities;
    {
        // Lock activities.json while reading and writing
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        std::ifstream f(activities_path);
        f >> activities;
        auto it = std::find_if(activities["activities"].begin(), activities["activities"].end(),
            [&created_at_time](const json &activity) {
                return activity["created_at"] == created_at_time;
            });
        if (it == activities["activities"].end()) {
            log("online ID " + online_id + " try to delete activity but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        // Remove activity
        activities["activities"].erase(it);
        std::ofstream out(activities_path);
        out << activities.dump(2);
    }

    log("online ID " + online_id + " deleted an activity");
    res.set_content("OK:Deleted", "text/plain");
}

void handle_get_activities(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "get activity", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    // Get target online_id from parameters
    std::string target_online_id = req.get_param_value("online_id");
    if (target_online_id.empty()) {
        log("Online ID " + online_id + " try to get activities with missing target online ID");
        res.set_content("ERR:MissingTargetOnlineID", "text/plain");
        return;
    }

    const std::string target_account_id = get_account_id_from_online_id(target_online_id);
    if (target_account_id.empty()) {
        log("Online ID " + online_id + " try to get activities for non-existing online ID " + target_online_id);
        res.set_content("ERR:TargetOnlineIDNotFound", "text/plain");
        return;
    }

    target_online_id = get_online_id_from_account_id(target_account_id);
    if (target_online_id.empty()) {
        log("Online ID " + online_id + " try to get activities for non-existing online ID " + target_online_id);
        res.set_content("ERR:TargetOnlineIDNotFound", "text/plain");
        return;
    }

    // Get language from parameters
    const auto language = req.get_param_value("sys_lang");
    if (language.empty()) {
        log("Online ID " + online_id + " try to get activities with missing language");
        res.set_content("ERR:MissingLanguage", "text/plain");
        return;
    }

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_online_id / "activities.json" };
    if (!fs::exists(activities_path)) {
        log("Online ID " + online_id + " try to get activities for online ID " + target_online_id + " but no activities found");
        res.set_content("ERR:NoActivities", "text/plain");
        return;
    }

    // Lock activities.json while reading
    json activities;
    {
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        std::ifstream f(activities_path);
        f >> activities;
    }

    // Create a copy of activities and enrich game activities with information from server database (title name, etc..)
    json result_activities;
    result_activities["activities"] = json::array();
    for (const auto &activity : activities["activities"]) {
        json entry = activity;
        auto it = entry.find("account_id");
        if (it == entry.end()) {
            log("Online ID " + online_id + " try to get activities for online ID " + target_online_id + " but activity has no account_id");
            continue;
        }

        const std::string activity_account_id = it->get<std::string>();
        const std::string activity_online_id = get_online_id_from_account_id(activity_account_id);

        entry.erase("account_id");
        entry["online_id"] = activity_online_id;
        if (activity.contains("friend_account_id")) {
            const std::string friend_account_id = activity["friend_account_id"].get<std::string>();
            const std::string friend_online_id = get_online_id_from_account_id(friend_account_id);
            entry.erase("friend_account_id");
            entry["friend_online_id"] = friend_online_id;
        }
        if (activity.contains("likes") && activity["likes"].is_array()) {
            json likes = json::array();
            for (const auto &like : activity["likes"]) {
                if (!like.is_string())
                    continue;

                const std::string like_account_id = like.get<std::string>();
                const std::string like_online_id = get_online_id_from_account_id(like_account_id);
                likes.push_back(like_online_id.empty() ? like_account_id : like_online_id);
            }
            entry["likes"] = likes;
        }
        if (activity.contains("comments") && activity["comments"].is_array()) {
            json comments = json::array();
            for (const auto &comment : activity["comments"]) {
                if (!comment.is_object())
                    continue;

                json comment_entry = comment;
                if (comment_entry.contains("account_id") && comment_entry["account_id"].is_string()) {
                    const std::string comment_online_id = get_online_id_from_account_id(comment_entry["account_id"].get<std::string>());
                    if (!comment_online_id.empty()) {
                        comment_entry.erase("account_id");
                        comment_entry["online_id"] = comment_online_id;
                    }
                }
                comments.push_back(comment_entry);
            }
            entry["comments"] = comments;
        }
        if (activity.contains("title_id")) {
            entry["title_name"] = get_stitle_name(activity["title_id"], language);
        }
        result_activities["activities"].push_back(entry);
    }

    log("Activities retrieved by online ID " + online_id + " for online ID " + target_online_id);
    res.set_content(result_activities.dump(), "application/json");
}
