// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>

void register_account_endpoints(httplib::Server &server);

void handle_check_connection(const httplib::Request &req, httplib::Response &res);
void handle_get_quota(const httplib::Request &req, httplib::Response &res);
void handle_create_account(const httplib::Request &req, httplib::Response &res);
void handle_delete_account(const httplib::Request &req, httplib::Response &res);
void handle_login(const httplib::Request &req, httplib::Response &res);
void handle_change_npid(const httplib::Request &req, httplib::Response &res);
void handle_change_password(const httplib::Request &req, httplib::Response &res);
void handle_upload_avatar(const httplib::Request &req, httplib::Response &res);
void handle_get_avatar(const httplib::Request &req, httplib::Response &res);
