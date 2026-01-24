// v3knr project
// Copyright (C) 2026 Vita3K team

#include "messages/messages.h"
#include "utils/utils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>

void register_messages_endpoints(httplib::Server &server) {
    server.Post("/v3kn/messages/create", handle_messages_create);
    server.Post("/v3kn/messages/send", handle_messages_send);
    server.Post("/v3kn/messages/delete", handle_messages_delete);
    server.Post("/v3kn/messages/add_participant", handle_messages_add_participant);
    server.Post("/v3kn/messages/leave", handle_messages_leave);
    server.Post("/v3kn/messages/delete_conversation", handle_messages_delete_conversation);
    server.Get("/v3kn/messages/conversations", handle_messages_conversations);
    server.Get("/v3kn/messages/read", handle_messages_read);
    server.Get("/v3kn/messages/poll", handle_messages_poll);
}

static void init_messages_fields(json &user) {
    if (!user.contains("messages") || !user["messages"].is_object())
        user["messages"] = json::object();
}

// Helper: Get conversation directory path
static std::string get_conversation_dir(const std::string &conversation_id) {
    fs::path conv_dir = fs::path("v3kn") / "conversations" / conversation_id;
    fs::create_directories(conv_dir);
    return conv_dir.string();
}

// Helper: Get conversation metadata file path
static std::string get_conversation_metadata_path(const std::string &conversation_id) {
    return (fs::path(get_conversation_dir(conversation_id)) / "metadata.json").string();
}

// Helper: Get conversation messages file path
static std::string get_conversation_messages_path(const std::string &conversation_id) {
    return (fs::path(get_conversation_dir(conversation_id)) / "messages.json").string();
}

// Helper: Load conversation metadata
static json load_conversation_metadata(const std::string &conversation_id) {
    std::string path = get_conversation_metadata_path(conversation_id);
    std::ifstream f(path);
    if (!f.is_open())
        return json::object();

    json metadata;
    f >> metadata;
    return metadata.is_object() ? metadata : json::object();
}

// Helper: Save conversation metadata
static void save_conversation_metadata(const std::string &conversation_id, const json &metadata) {
    std::string path = get_conversation_metadata_path(conversation_id);
    std::ofstream f(path);
    f << metadata.dump(2);
}

// Helper: Load conversation messages
static json load_conversation_messages(const std::string &conversation_id) {
    std::string path = get_conversation_messages_path(conversation_id);
    std::ifstream f(path);
    if (!f.is_open())
        return json::array();

    json messages;
    f >> messages;
    return messages.is_array() ? messages : json::array();
}

// Helper: Save conversation messages
static void save_conversation_messages(const std::string &conversation_id, const json &messages) {
    std::string path = get_conversation_messages_path(conversation_id);
    std::ofstream f(path);
    f << messages.dump(2);
}

// Helper: Get user's conversations file path
static std::string get_user_conversations_path(const std::string &npid) {
    fs::path dir = fs::path("v3kn") / "Users" / npid;
    fs::create_directories(dir);
    return (dir / "conversations.json").string();
}

// Helper: Load user's conversations
static json load_user_conversations(const std::string &npid) {
    std::string path = get_user_conversations_path(npid);
    std::ifstream f(path);
    if (!f.is_open())
        return json::array();

    json conversations;
    f >> conversations;
    return conversations.is_array() ? conversations : json::array();
}

// Helper: Save user's conversations
static void save_user_conversations(const std::string &npid, const json &conversations) {
    std::string path = get_user_conversations_path(npid);
    std::ofstream f(path);
    f << conversations.dump(2);
}

// Helper: Generate conversation ID from participants
static std::string generate_conversation_id(const std::vector<std::string> &participants) {
    // Sort participants for consistent ID
    std::vector<std::string> sorted = participants;
    std::sort(sorted.begin(), sorted.end());

    if (sorted.size() == 2) {
        // 1-on-1: Alice_Bob
        return sorted[0] + "_" + sorted[1];
    } else {
        // Group: group_{hash}
        std::string combined;
        for (const auto &p : sorted) {
            combined += p;
        }
        // Simple hash (timestamp + participants)
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return "group_" + std::to_string(std::hash<std::string>{}(combined + std::to_string(timestamp)));
    }
}

// Helper: Check if user is in conversation
static bool is_user_in_conversation(const std::string &conversation_id, const std::string &npid) {
    json metadata = load_conversation_metadata(conversation_id);
    if (!metadata.contains("participants") || !metadata["participants"].is_array())
        return false;

    for (const auto &p : metadata["participants"]) {
        if (p.is_string() && p.get<std::string>() == npid)
            return true;
    }
    return false;
}

void handle_messages_create(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "create conversation request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Parse JSON body
    json request_data;
    try {
        request_data = json::parse(req.body);
    } catch (...) {
        log("Invalid JSON in create conversation request from " + npid);
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    if (!request_data.contains("participants") || !request_data["participants"].is_array()) {
        log("Missing or invalid participants in create conversation request from " + npid);
        res.set_content("ERR:MissingParticipants", "text/plain");
        return;
    }

    if (!request_data.contains("message") || !request_data["message"].is_string()) {
        log("Missing message in create conversation request from " + npid);
        res.set_content("ERR:MissingMessage", "text/plain");
        return;
    }

    const std::string first_message = request_data["message"].get<std::string>();
    if (first_message.empty() || first_message.size() > 2000) {
        log("Invalid message in create conversation request from " + npid);
        res.set_content("ERR:InvalidMessage", "text/plain");
        return;
    }

    // Extract participants (include creator)
    std::vector<std::string> participants;
    participants.push_back(npid); // Creator is always a participant

    for (const auto &p : request_data["participants"]) {
        if (!p.is_string()) {
            log("Invalid participant in create conversation request from " + npid);
            res.set_content("ERR:InvalidParticipant", "text/plain");
            return;
        }
        std::string participant = trim_npid(p.get<std::string>());
        if (!participant.empty() && participant != npid) {
            participants.push_back(participant);
        }
    }

    if (participants.size() < 2) {
        log("Conversation must have at least 2 participants (from " + npid + ")");
        res.set_content("ERR:NotEnoughParticipants", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    // Verify all participants exist
    for (const auto &p : participants) {
        if (!db["users"].contains(p)) {
            log("Create conversation request with non-existing participant " + p + " by " + npid);
            res.set_content("ERR:ParticipantNotFound:" + p, "text/plain");
            return;
        }
    }

    // Generate conversation ID
    const std::string conversation_id = generate_conversation_id(participants);

    // Check if conversation already exists
    json metadata = load_conversation_metadata(conversation_id);
    if (!metadata.empty()) {
        log("Conversation " + conversation_id + " already exists");
        res.set_content("ERR:ConversationAlreadyExists", "text/plain");
        return;
    }

    // Create conversation metadata
    metadata["conversation_id"] = conversation_id;
    metadata["participants"] = participants;
    metadata["creator"] = npid;
    metadata["created_at"] = std::time(0);

    save_conversation_metadata(conversation_id, metadata);

    // Create first message
    json messages = json::array();
    json msg;
    msg["from"] = npid;
    msg["msg"] = first_message;
    msg["timestamp"] = std::time(0);
    messages.push_back(msg);

    save_conversation_messages(conversation_id, messages);

    // Add conversation to each participant's list
    for (const auto &p : participants) {
        json user_conversations = load_user_conversations(p);
        if (std::find(user_conversations.begin(), user_conversations.end(), conversation_id) == user_conversations.end()) {
            user_conversations.push_back(conversation_id);
            save_user_conversations(p, user_conversations);
        }
    }

    // Notify all waiting polls
    messages_cv.notify_all();

    log("Conversation created: " + conversation_id + " by " + npid + " with " + std::to_string(participants.size()) + " participants");
    res.set_content("OK:" + conversation_id, "text/plain");
}

void handle_messages_send(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "message send request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string conversation_id = trim_npid(req.get_param_value("conversation_id"));
    if (conversation_id.empty()) {
        log("Missing conversation_id on message send request for NPID " + npid);
        res.set_content("ERR:MissingConversationID", "text/plain");
        return;
    }

    const std::string message = req.get_param_value("message");
    if (message.empty()) {
        log("Missing message on message send request for NPID " + npid);
        res.set_content("ERR:MissingMessage", "text/plain");
        return;
    }

    if (message.size() > 2000) {
        log("Message too long from " + npid + " in conversation " + conversation_id);
        res.set_content("ERR:MessageTooLong", "text/plain");
        return;
    }

    // Load conversation metadata
    json metadata = load_conversation_metadata(conversation_id);
    if (metadata.empty()) {
        log("Message send to non-existing conversation " + conversation_id + " by " + npid);
        res.set_content("ERR:ConversationNotFound", "text/plain");
        return;
    }

    // Verify sender is in the conversation
    if (!is_user_in_conversation(conversation_id, npid)) {
        log("Message send to conversation " + conversation_id + " by non-member " + npid);
        res.set_content("ERR:NotInConversation", "text/plain");
        return;
    }

    // Load conversation messages
    json messages = load_conversation_messages(conversation_id);

    // Create message object
    json msg;
    msg["from"] = npid;
    msg["msg"] = message;
    msg["timestamp"] = std::time(0);

    // Append to conversation messages
    messages.push_back(msg);

    // Save conversation messages
    save_conversation_messages(conversation_id, messages);

    // Notify all waiting polls
    messages_cv.notify_all();

    log("Message sent from " + npid + " to conversation " + conversation_id);
    res.set_content("OK:MessageSent", "text/plain");
}

void handle_messages_delete(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "message delete request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Parse JSON body
    json request_data;
    try {
        request_data = json::parse(req.body);
    } catch (...) {
        log("Invalid JSON in message delete request from " + npid);
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    if (!request_data.contains("conversation_id") || !request_data["conversation_id"].is_string()) {
        log("Missing conversation_id in message delete request from " + npid);
        res.set_content("ERR:MissingConversationID", "text/plain");
        return;
    }

    if (!request_data.contains("timestamps") || !request_data["timestamps"].is_array()) {
        log("Missing or invalid timestamps in message delete request from " + npid);
        res.set_content("ERR:MissingTimestamps", "text/plain");
        return;
    }

    const std::string conversation_id = request_data["conversation_id"].get<std::string>();
    if (conversation_id.empty()) {
        log("Empty conversation_id in message delete request from " + npid);
        res.set_content("ERR:EmptyConversationID", "text/plain");
        return;
    }

    // Extract timestamps array
    std::vector<int64_t> timestamps;
    for (const auto &ts : request_data["timestamps"]) {
        if (!ts.is_number_integer()) {
            log("Invalid timestamp in delete request from " + npid);
            res.set_content("ERR:InvalidTimestamp", "text/plain");
            return;
        }
        timestamps.push_back(ts.get<int64_t>());
    }

    if (timestamps.empty()) {
        log("No timestamps provided in delete request from " + npid);
        res.set_content("ERR:NoTimestamps", "text/plain");
        return;
    }

    // Load conversation metadata
    json metadata = load_conversation_metadata(conversation_id);
    if (metadata.empty()) {
        log("Message delete request to non-existing conversation " + conversation_id + " by " + npid);
        res.set_content("ERR:ConversationNotFound", "text/plain");
        return;
    }

    // Verify requester is in the conversation
    if (!is_user_in_conversation(conversation_id, npid)) {
        log("Message delete request to conversation " + conversation_id + " by non-member " + npid);
        res.set_content("ERR:NotInConversation", "text/plain");
        return;
    }

    // Load conversation messages
    json messages = load_conversation_messages(conversation_id);
    int deleted_count = 0;

    // Delete messages by timestamp (only if sent by this user)
    for (const auto &timestamp : timestamps) {
        for (auto it = messages.begin(); it != messages.end(); ++it) {
            if (it->contains("timestamp") && it->contains("from") && 
                (*it)["timestamp"].get<int64_t>() == timestamp) {

                // Verify the message is from this user
                if ((*it)["from"].get<std::string>() != npid) {
                    log("User " + npid + " tried to delete message not sent by them in conversation " + conversation_id);
                    break;
                }

                messages.erase(it);
                deleted_count++;
                break;
            }
        }
    }

    if (deleted_count == 0) {
        log("No messages deleted for " + npid + " in conversation " + conversation_id);
        res.set_content("ERR:NoMessagesDeleted", "text/plain");
        return;
    }

    // Save conversation messages
    save_conversation_messages(conversation_id, messages);

    // Notify all waiting polls
    messages_cv.notify_all();

    log("Messages deleted by " + npid + " in conversation " + conversation_id + " (count: " + std::to_string(deleted_count) + ")");
    res.set_content("OK:MessagesDeleted:" + std::to_string(deleted_count), "text/plain");
}

void handle_messages_add_participant(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "add participant request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Parse JSON body
    json request_data;
    try {
        request_data = json::parse(req.body);
    } catch (...) {
        log("Invalid JSON in add participant request from " + npid);
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    if (!request_data.contains("conversation_id") || !request_data["conversation_id"].is_string()) {
        log("Missing conversation_id in add participant request from " + npid);
        res.set_content("ERR:MissingConversationID", "text/plain");
        return;
    }

    if (!request_data.contains("participant") || !request_data["participant"].is_string()) {
        log("Missing participant in add participant request from " + npid);
        res.set_content("ERR:MissingParticipant", "text/plain");
        return;
    }

    const std::string conversation_id = request_data["conversation_id"].get<std::string>();
    const std::string new_participant = trim_npid(request_data["participant"].get<std::string>());

    if (new_participant.empty()) {
        log("Empty participant in add participant request from " + npid);
        res.set_content("ERR:EmptyParticipant", "text/plain");
        return;
    }

    std::lock_guard<std::mutex> lock_db(account_mutex);
    json db = load_users();

    // Verify new participant exists
    if (!db["users"].contains(new_participant)) {
        log("Add participant request with non-existing user " + new_participant + " by " + npid);
        res.set_content("ERR:ParticipantNotFound", "text/plain");
        return;
    }

    // Load conversation metadata
    json metadata = load_conversation_metadata(conversation_id);
    if (metadata.empty() || !metadata.contains("participants")) {
        log("Add participant request to non-existing conversation " + conversation_id + " by " + npid);
        res.set_content("ERR:ConversationNotFound", "text/plain");
        return;
    }

    // Verify requester is in the conversation
    if (!is_user_in_conversation(conversation_id, npid)) {
        log("Add participant request to conversation " + conversation_id + " by non-member " + npid);
        res.set_content("ERR:NotInConversation", "text/plain");
        return;
    }

    // Check if participant is already in conversation
    if (is_user_in_conversation(conversation_id, new_participant)) {
        log("Participant " + new_participant + " already in conversation " + conversation_id);
        res.set_content("ERR:AlreadyInConversation", "text/plain");
        return;
    }

    // Add participant to conversation
    metadata["participants"].push_back(new_participant);
    save_conversation_metadata(conversation_id, metadata);

    // Add conversation to new participant's list
    json user_conversations = load_user_conversations(new_participant);
    if (std::find(user_conversations.begin(), user_conversations.end(), conversation_id) == user_conversations.end()) {
        user_conversations.push_back(conversation_id);
        save_user_conversations(new_participant, user_conversations);
    }

    // Notify all waiting polls
    messages_cv.notify_all();

    log("Participant " + new_participant + " added to conversation " + conversation_id + " by " + npid);
    res.set_content("OK:ParticipantAdded", "text/plain");
}

void handle_messages_leave(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "leave conversation request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Parse JSON body
    json request_data;
    try {
        request_data = json::parse(req.body);
    } catch (...) {
        log("Invalid JSON in leave conversation request from " + npid);
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    if (!request_data.contains("conversation_id") || !request_data["conversation_id"].is_string()) {
        log("Missing conversation_id in leave conversation request from " + npid);
        res.set_content("ERR:MissingConversationID", "text/plain");
        return;
    }

    const std::string conversation_id = request_data["conversation_id"].get<std::string>();

    // Load conversation metadata
    json metadata = load_conversation_metadata(conversation_id);
    if (metadata.empty() || !metadata.contains("participants")) {
        log("Leave conversation request to non-existing conversation " + conversation_id + " by " + npid);
        res.set_content("ERR:ConversationNotFound", "text/plain");
        return;
    }

    // Verify requester is in the conversation
    if (!is_user_in_conversation(conversation_id, npid)) {
        log("Leave conversation request to conversation " + conversation_id + " by non-member " + npid);
        res.set_content("ERR:NotInConversation", "text/plain");
        return;
    }

    // Remove participant from conversation
    auto &participants = metadata["participants"];
    participants.erase(
        std::remove_if(participants.begin(), participants.end(),
            [&npid](const json &p) {
                return p.is_string() && p.get<std::string>() == npid;
            }),
        participants.end()
    );

    save_conversation_metadata(conversation_id, metadata);

    // Remove conversation from user's list
    json user_conversations = load_user_conversations(npid);
    user_conversations.erase(
        std::remove(user_conversations.begin(), user_conversations.end(), conversation_id),
        user_conversations.end()
    );
    save_user_conversations(npid, user_conversations);

    // Notify all waiting polls
    messages_cv.notify_all();

    log("User " + npid + " left conversation " + conversation_id);
    res.set_content("OK:LeftConversation", "text/plain");
}

void handle_messages_delete_conversation(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "delete conversation request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Parse JSON body
    json request_data;
    try {
        request_data = json::parse(req.body);
    } catch (...) {
        log("Invalid JSON in delete conversation request from " + npid);
        res.set_content("ERR:InvalidJSON", "text/plain");
        return;
    }

    if (!request_data.contains("conversation_id") || !request_data["conversation_id"].is_string()) {
        log("Missing conversation_id in delete conversation request from " + npid);
        res.set_content("ERR:MissingConversationID", "text/plain");
        return;
    }

    const std::string conversation_id = request_data["conversation_id"].get<std::string>();

    // Load conversation metadata
    json metadata = load_conversation_metadata(conversation_id);
    if (metadata.empty()) {
        log("Delete conversation request to non-existing conversation " + conversation_id + " by " + npid);
        res.set_content("ERR:ConversationNotFound", "text/plain");
        return;
    }

    // Verify requester is the creator
    if (!metadata.contains("creator") || metadata["creator"].get<std::string>() != npid) {
        log("Delete conversation request to conversation " + conversation_id + " by non-creator " + npid);
        res.set_content("ERR:NotCreator", "text/plain");
        return;
    }

    // Remove conversation from all participants' lists
    if (metadata.contains("participants") && metadata["participants"].is_array()) {
        for (const auto &p : metadata["participants"]) {
            if (!p.is_string()) continue;
            std::string participant = p.get<std::string>();

            json user_conversations = load_user_conversations(participant);
            user_conversations.erase(
                std::remove(user_conversations.begin(), user_conversations.end(), conversation_id),
                user_conversations.end()
            );
            save_user_conversations(participant, user_conversations);
        }
    }

    // Delete conversation files
    fs::remove_all(get_conversation_dir(conversation_id));

    // Notify all waiting polls
    messages_cv.notify_all();

    log("Conversation " + conversation_id + " deleted by creator " + npid);
    res.set_content("OK:ConversationDeleted", "text/plain");
}

void handle_messages_conversations(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "conversations list request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    // Build JSON array of conversations
    json response = json::array();

    // Get all user's conversations
    json user_conversations = load_user_conversations(npid);

    for (const auto &conv_id_json : user_conversations) {
        if (!conv_id_json.is_string())
            continue;

        std::string conversation_id = conv_id_json.get<std::string>();
        json metadata = load_conversation_metadata(conversation_id);

        if (metadata.empty())
            continue;

        json messages = load_conversation_messages(conversation_id);

        json conv;
        conv["npid"] = conversation_id; // Use conversation_id as identifier
        conv["count"] = messages.size();
        conv["creator"] = metadata.value("creator", "");
        conv["participants"] = metadata.value("participants", json::array());

        if (!messages.empty()) {
            conv["last_message"] = messages.back();
        }

        response.push_back(conv);
    }

    log("Conversations list requested by " + npid + " (" + std::to_string(response.size()) + " conversations)");
    res.set_content(response.dump(), "application/json");
}

void handle_messages_read(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "messages read request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string conversation_id = trim_npid(req.get_param_value("conversation_id"));
    if (conversation_id.empty()) {
        log("Missing conversation_id on messages read request for NPID " + npid);
        res.set_content("ERR:MissingConversationID", "text/plain");
        return;
    }

    // Load conversation metadata
    json metadata = load_conversation_metadata(conversation_id);
    if (metadata.empty()) {
        log("Messages read request to non-existing conversation " + conversation_id + " by " + npid);
        res.set_content("ERR:ConversationNotFound", "text/plain");
        return;
    }

    // Verify requester is in the conversation
    if (!is_user_in_conversation(conversation_id, npid)) {
        log("Messages read request to conversation " + conversation_id + " by non-member " + npid);
        res.set_content("ERR:NotInConversation", "text/plain");
        return;
    }

    // Load conversation messages
    json messages = load_conversation_messages(conversation_id);

    log("Messages read: " + npid + " <-> conversation " + conversation_id + " (" + std::to_string(messages.size()) + " messages)");
    res.set_content(messages.dump(), "application/json");
}

void handle_messages_poll(const httplib::Request &req, httplib::Response &res) {
    std::string err;
    const std::string npid = get_valid_npid(req, "messages poll request", err);
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

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(30);

    while (true) {
        json new_messages = json::array();

        {
            // Check all user's conversations
            json user_conversations = load_user_conversations(npid);

            for (const auto &conv_id_json : user_conversations) {
                if (!conv_id_json.is_string())
                    continue;

                std::string conversation_id = conv_id_json.get<std::string>();
                json messages = load_conversation_messages(conversation_id);

                for (const auto &msg : messages) {
                    if (!msg.contains("timestamp")) continue;
                    if (!msg.contains("from")) continue;

                    int64_t msg_timestamp = msg["timestamp"].get<int64_t>();
                    std::string msg_from = msg["from"].get<std::string>();

                    // Only return messages RECEIVED (not sent by this user)
                    if (msg_timestamp > since_timestamp && msg_from != npid) {
                        new_messages.push_back(msg);
                    }
                }
            }
        }

        if (!new_messages.empty()) {
            log("Poll: " + npid + " - " + std::to_string(new_messages.size()) + " new messages");
            res.set_content(new_messages.dump(), "application/json");
            return;
        }

        // Calculate remaining time
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto remaining = timeout - elapsed;

        if (remaining <= std::chrono::seconds(0)) {
            json empty = json::array();
            res.set_content(empty.dump(), "application/json");
            return;
        }

        // Wait for notification or timeout
        std::unique_lock<std::mutex> lock(messages_cv_mutex);
        messages_cv.wait_for(lock, remaining);
    }

    json empty = json::array();
    res.set_content(empty.dump(), "application/json");
}
