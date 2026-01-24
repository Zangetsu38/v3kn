// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <httplib.h>

void register_messages_endpoints(httplib::Server &server);

void handle_messages_create(const httplib::Request &req, httplib::Response &res);
void handle_messages_send(const httplib::Request &req, httplib::Response &res);
void handle_messages_delete(const httplib::Request &req, httplib::Response &res);
void handle_messages_add_participant(const httplib::Request &req, httplib::Response &res);
void handle_messages_leave(const httplib::Request &req, httplib::Response &res);
void handle_messages_delete_conversation(const httplib::Request &req, httplib::Response &res);
void handle_messages_conversations(const httplib::Request &req, httplib::Response &res);
void handle_messages_read(const httplib::Request &req, httplib::Response &res);
void handle_messages_poll(const httplib::Request &req, httplib::Response &res);
