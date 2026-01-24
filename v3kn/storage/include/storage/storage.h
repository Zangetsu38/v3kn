// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>

void register_storage_endpoints(httplib::Server &server);

void handle_get_save_info(const httplib::Request &req, httplib::Response &res);
void handle_get_trophies_info(const httplib::Request &req, httplib::Response &res);
void handle_download_file(const httplib::Request &req, httplib::Response &res);
void handle_upload_file(const httplib::Request &req, httplib::Response &res);
