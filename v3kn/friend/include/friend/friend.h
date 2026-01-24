// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>

void register_friends_endpoints(httplib::Server &server);

void handle_friend_add(const httplib::Request &req, httplib::Response &res);
void handle_friend_accept(const httplib::Request &req, httplib::Response &res);
void handle_friend_reject(const httplib::Request &req, httplib::Response &res);
void handle_friend_remove(const httplib::Request &req, httplib::Response &res);
void handle_friend_cancel(const httplib::Request &req, httplib::Response &res);
void handle_friend_block(const httplib::Request &req, httplib::Response &res);
void handle_friend_unblock(const httplib::Request &req, httplib::Response &res);
void handle_friend_list(const httplib::Request &req, httplib::Response &res);
void handle_friend_profile(const httplib::Request &req, httplib::Response &res);
void handle_friend_poll(const httplib::Request &req, httplib::Response &res);
void handle_friend_presence(const httplib::Request &req, httplib::Response &res);
void handle_friend_search(const httplib::Request &req, httplib::Response &res);

void notify_avatar_changed(const std::string &npid);
