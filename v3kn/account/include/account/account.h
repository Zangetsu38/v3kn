// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>

void update_profile_timestamp(const std::string &online_id);

void register_account_endpoints(httplib::Server &server);

void handle_check_connection(const httplib::Request &req, httplib::Response &res);
void handle_get_quota(const httplib::Request &req, httplib::Response &res);
void handle_create_account(const httplib::Request &req, httplib::Response &res);
void handle_delete_account(const httplib::Request &req, httplib::Response &res);
void handle_login(const httplib::Request &req, httplib::Response &res);
void handle_change_online_id(const httplib::Request &req, httplib::Response &res);
void handle_change_password(const httplib::Request &req, httplib::Response &res);
void handle_change_about_me(const httplib::Request &req, httplib::Response &res);
void handle_upload_avatar(const httplib::Request &req, httplib::Response &res);
void handle_get_avatar(const httplib::Request &req, httplib::Response &res);
void handle_upload_panel(const httplib::Request &req, httplib::Response &res);
void handle_get_panel(const httplib::Request &req, httplib::Response &res);
