// v3knr project
// Copyright (C) 2026 Vita3K team

#include <account/account.h>
#include <activity/activity.h>
#include <utils/utils.h>

#include <ctime>
#include <fstream>

static std::mutex activities_mutex;
static void append_activity(const std::string &npid, const json &activity) {
    const fs::path activities_path{ fs::path("v3kn") / "Users" / npid / "activities.json" };
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
        f << activities.dump(4);
    }

    // Update profile timestamp
    update_profile_timestamp(npid);

    log("Activity of type " + activity["type"].get<std::string>() + " posted for NPID " + npid);
}

void create_friendship_established_activity(const std::string &npid1, const std::string &npid2) {
    // Create activity for npid1
    {
        json payload;
        payload["type"] = "friendship_established";
        payload["online_id"] = npid1;
        payload["friend_id"] = npid2;
        payload["created_at"] = std::time(0);
        append_activity(npid1, payload);
    }

    // Create activity for npid2
    {
        json payload;
        payload["type"] = "friendship_established";
        payload["online_id"] = npid2;
        payload["friend_id"] = npid1;
        payload["created_at"] = std::time(0);
        append_activity(npid2, payload);
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
    const std::string npid = get_valid_npid(req, "post activity", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Parse JSON
    json payload;
    try {
        payload = json::parse(req.body);
    } catch (...) {
        log("NPID " + npid + " try to post activity with invalid JSON");
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    // Post activity
    append_activity(npid, payload);

    // Respond
    res.set_content("OK:ActivityPosted", "text/plain");
}

void handle_like_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "like activity", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("NPID " + npid + " try to like activity with missing target NPID");
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    const auto created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("NPID " + npid + " try to like activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    // Convert created_at to time_t
    const time_t created_at_time = static_cast<time_t>(std::stoll(created_at));

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_npid / "activities.json" };
    json activities;
    {
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        if (!fs::exists(activities_path)) {
            log("NPID " + npid + " try to like activity for NPID " + target_npid + " but no activities found");
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
            log("NPID " + npid + " try to like activity for NPID " + target_npid + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        auto &likes = (*it)["likes"];

        // Check if already has 100 likes
        if (likes.size() >= 100) {
            log("NPID " + npid + " try to like activity for NPID " + target_npid + " but already has 100 likes");
            res.set_content("ERR:MaxLikesReached", "text/plain");
            return;
        }

        // Add like if not already liked
        if (std::find(likes.begin(), likes.end(), npid) == likes.end()) {
            likes.push_back(npid);
        } else {
            log("NPID " + npid + " try to like activity for NPID " + target_npid + " but already liked");
            res.set_content("ERR:AlreadyLiked", "text/plain");
            return;
        }

        // Save updated activities.json
        std::ofstream f(activities_path);
        f << activities.dump(4);
    }

    log("NPID " + npid + " liked activity for NPID " + target_npid);
    res.set_content("OK", "text/plain");
}

void handle_unlike_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "unlike activity", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("NPID " + npid + " try to unlike activity with missing target NPID");
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    const std::string created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("NPID " + npid + " try to unlike activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    // Convert created_at to time_t
    const time_t created_at_time = static_cast<time_t>(std::stoll(created_at));

    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_npid / "activities.json" };
    json activities;

    if (!fs::exists(activities_path)) {
        log("NPID " + npid + " try to unlike activity for NPID " + target_npid + " but no activities found");
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
            log("NPID " + npid + " try to unlike activity for NPID " + target_npid + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        auto &likes = (*it)["likes"];
        auto like_it = std::find(likes.begin(), likes.end(), npid);

        if (like_it == likes.end()) {
            log("NPID " + npid + " try to unlike activity for NPID " + target_npid + " but activity not liked");
            res.set_content("ERR:NotLiked", "text/plain");
            return;
        }

        likes.erase(like_it);

        std::ofstream out(activities_path);
        out << activities.dump(4);
    }

    res.set_content("OK", "text/plain");
}

void handle_comment_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const std::string npid = get_valid_npid(req, "comment activity", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("NPID " + npid + " try to comment activity with missing target NPID");
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    const auto created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("NPID " + npid + " try to comment activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    // Convert created_at to time_t
    const time_t created_at_time = static_cast<time_t>(std::stoll(created_at));

    const auto comment = req.get_param_value("comment");
    if (comment.empty()) {
        log("NPID " + npid + " try to comment activity with missing comment");
        res.set_content("ERR:MissingComment", "text/plain");
        return;
    }

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_npid / "activities.json" };
    json activities;
    {
        std::lock_guard<std::mutex> activities_lock(activities_mutex);
        if (!fs::exists(activities_path)) {
            log("NPID " + npid + " try to comment activity for NPID " + target_npid + " but no activities found");
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
            log("NPID " + npid + " try to comment activity for NPID " + target_npid + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        // Add comment
        json comment_entry;
        comment_entry["npid"] = npid;
        comment_entry["comment"] = comment;
        comment_entry["created_at"] = std::time(0);

        // Add comment to activity
        auto &comments = (*it)["comments"];
        comments.push_back(comment_entry);

        // Keep only latest 20 comments
        if (comments.size() > 20) {
            const auto diff = comments.size() - 20;
            comments.erase(comments.begin(), comments.begin() + diff);
        }

        // Save updated activities.json
        std::ofstream f(activities_path);
        f << activities.dump(4);
    }
}

void handle_uncomment_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const std::string npid = get_valid_npid(req, "uncomment activity", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Get target NPID and created_at from parameters
    const std::string target_npid = trim_npid(req.get_param_value("target_npid"));
    if (target_npid.empty()) {
        log("NPID " + npid + " try to uncomment activity with missing target NPID");
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    // Get created_at from parameters
    const std::string created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("NPID " + npid + " try to uncomment activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    // Convert created_at to time_t
    const time_t created_at_time = static_cast<time_t>(std::stoll(created_at));

    // Get comment_created_at from parameters
    const std::string comment_created_at = req.get_param_value("comment_created_at");
    if (comment_created_at.empty()) {
        log("NPID " + npid + " try to uncomment activity with missing comment_created_at");
        res.set_content("ERR:MissingCommentCreatedAt", "text/plain");
        return;
    }

    // Convert comment_created_at to time_t
    const time_t comment_created_at_time = static_cast<time_t>(std::stoll(comment_created_at));

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_npid / "activities.json" };
    if (!fs::exists(activities_path)) {
        log("NPID " + npid + " try to uncomment activity for NPID " + target_npid + " but no activities found");
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
            log("NPID " + npid + " try to uncomment activity for NPID " + target_npid + " but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        // Find comment by comment_created_at timestamp
        auto &comments = (*it)["comments"];
        auto comment_it = std::find_if(comments.begin(), comments.end(),
            [&npid, &comment_created_at_time](const json &comment) {
                return comment["created_at"] == comment_created_at_time;
            });
        if (comment_it == comments.end()) {
            log("NPID " + npid + " try to uncomment activity for NPID " + target_npid + " but no matching comment found");
            res.set_content("ERR:CommentNotFound", "text/plain");
            return;
        }

        // Check if the comment author is the same as the requester or if the requester is the owner of the activity
        const std::string author = (*comment_it)["npid"];
        if ((author != npid) && (npid != target_npid)) {
            log("NPID " + npid + " try to delete comment for NPID " + target_npid + " but not allowed to delete this comment");
            res.set_content("ERR:NotAllowed", "text/plain");
            return;
        }

        // Remove comment
        comments.erase(comment_it);
        std::ofstream out(activities_path);
        out << activities.dump(4);
    }
}

void handle_delete_activity(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const std::string npid = get_valid_npid(req, "delete activity", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Get created_at from parameters
    const std::string created_at = req.get_param_value("created_at");
    if (created_at.empty()) {
        log("NPID " + npid + " try to delete activity with missing created_at");
        res.set_content("ERR:MissingCreatedAt", "text/plain");
        return;
    }

    // Convert created_at to time_t
    const time_t created_at_time = static_cast<time_t>(std::stoll(created_at));

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / npid / "activities.json" };
    if (!fs::exists(activities_path)) {
        log("NPID " + npid + " try to delete activity but no activities found");
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
            log("NPID " + npid + " try to delete activity but no matching activity found");
            res.set_content("ERR:ActivityNotFound", "text/plain");
            return;
        }

        // Remove activity
        activities["activities"].erase(it);
        std::ofstream out(activities_path);
        out << activities.dump(4);
    }

    log("NPID " + npid + " deleted an activity");
    res.set_content("OK", "text/plain");
}

void handle_get_activities(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "get activity", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Get target NPID from parameters
    const auto target_npid = req.get_param_value("npid");
    if (target_npid.empty()) {
        log("NPID " + npid + " try to get activities with missing target NPID");
        res.set_content("ERR:MissingTargetNPID", "text/plain");
        return;
    }

    // Get language from parameters
    const auto language = req.get_param_value("sys_lang");
    if (language.empty()) {
        log("NPID " + npid + " try to get activities with missing language");
        res.set_content("ERR:MissingLanguage", "text/plain");
        return;
    }

    // Load activities.json
    const fs::path activities_path{ fs::path("v3kn") / "Users" / target_npid / "activities.json" };
    if (!fs::exists(activities_path)) {
        log("NPID " + npid + " try to get activities for NPID " + target_npid + " but no activities found");
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
        if (activity.contains("title_id")) {
            entry["title_name"] = get_title_name(activity["title_id"], language);
        }
        result_activities["activities"].push_back(entry);
    }

    log("Activities retrieved for NPID " + target_npid);
    res.set_content(result_activities.dump(), "application/json");
}
