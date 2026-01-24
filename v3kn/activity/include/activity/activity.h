// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>

void create_friendship_established_activity(const std::string &account_id1, const std::string &account_id2);
void migrate_activities_npid_to_account_id();

void register_activity_endpoints(httplib::Server &server);

void handle_post_activity(const httplib::Request &req, httplib::Response &res);
void handle_like_activity(const httplib::Request &req, httplib::Response &res);
void handle_unlike_activity(const httplib::Request &req, httplib::Response &res);
void handle_comment_activity(const httplib::Request &req, httplib::Response &res);
void handle_uncomment_activity(const httplib::Request &req, httplib::Response &res);
void handle_delete_activity(const httplib::Request &req, httplib::Response &res);
void handle_get_activities(const httplib::Request &req, httplib::Response &res);
