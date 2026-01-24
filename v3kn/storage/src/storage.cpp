// v3knr project
// Copyright (C) 2026 Vita3K team

#include "storage/storage.h"
#include "utils/utils.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

void register_storage_endpoints(httplib::Server &server) {
    server.Get("/v3kn/save_info", handle_get_save_info);
    server.Get("/v3kn/trophies_info", handle_get_trophies_info);
    server.Get("/v3kn/download_file", handle_download_file);
    server.Post("/v3kn/upload_file", handle_upload_file);
}

void handle_get_save_info(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "save info request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    auto titleid = req.get_param_value("titleid");
    if (titleid.empty()) {
        log("Missing TitleID on save info request for NPID " + npid);
        res.set_content("ERR:MissingTitleID", "text/plain");
        return;
    }

    log("NPID: " + npid + ", TitleID: " + titleid);

    const std::string savedata_path = "v3kn/Users/" + npid + "/savedata/" + titleid;
    if (!fs::exists(savedata_path)) {
        log("No savedata for NPID " + npid + " TitleID " + titleid);
        res.set_content("WARN:NoSavedata", "text/plain");
        return;
    }

    std::ifstream savedata_info_file(fs::path(savedata_path) / "savedata.xml");
    if (!savedata_info_file) {
        log("No savedata info file for NPID " + npid + " TitleID " + titleid);
        res.set_content("WARN:NoSavedataInfo", "text/plain");
        return;
    }

    std::string savedata_content((std::istreambuf_iterator<char>(savedata_info_file)),
        std::istreambuf_iterator<char>());

    update_last_activity(req, npid);
    res.set_content(savedata_content, "application/xml");
}

void handle_get_trophies_info(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "trophies info request", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    log("NPID: " + npid + " requesting trophies info");

    std::ifstream trophies_info_file(fs::path("v3kn") / "Users" / npid / "trophy" / "trophies.xml");
    if (!trophies_info_file) {
        log("No trophies info file for NPID " + npid);
        res.set_content("WARN:NoTrophiesInfo", "text/plain");
        return;
    }

    std::string trophies_content((std::istreambuf_iterator<char>(trophies_info_file)),
        std::istreambuf_iterator<char>());

    update_last_activity(req, npid);
    res.set_content(trophies_content, "application/xml");
}

void handle_download_file(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "file download", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const auto type = req.get_param_value("type");
    if ((type != "savedata") && (type != "trophy")) {
        log("NPID " + npid + " try to download with invalid type: " + type);
        res.set_content("ERR:InvalidType", "text/plain");
        return;
    }

    const auto id = req.get_param_value("id");
    bool invalid_id = false;
    if (type == "savedata")
        invalid_id = !id.starts_with("PCS") || (id.size() != 9);
    else if (type == "trophy")
        invalid_id = !id.starts_with("NPWR") || (id.size() != 12);

    if (invalid_id) {
        log("NPID " + npid + " try to download with invalid id: " + id);
        res.set_content("ERR:InvalidID", "text/plain");
        return;
    }

    auto msg = "NPID: " + npid + " type: " + type + " id: " + id;

    const auto path = (type == "savedata") ? "savedata.psvimg" : "TROPUSR.DAT";
    const fs::path file_path{ fs::path("v3kn") / "Users" / npid / type / id / path };

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        log(msg + ", File not found: " + file_path.string());
        res.set_content("ERR:FileNotFound", "text/plain");
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    msg += "\nServing file: " + file_path.string() + " (" + std::to_string(buffer.str().size()) + " bytes)";
    log(msg);

    update_last_activity(req, npid);
    res.set_content(buffer.str(), "application/octet-stream");
}

void handle_upload_file(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "file upload", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const auto type = req.get_param_value("type");
    if ((type != "savedata") && (type != "trophy")) {
        log("NPID " + npid + " try to upload with invalid type: " + type);
        res.set_content("ERR:InvalidType", "text/plain");
        return;
    }

    const auto id = req.get_param_value("id");
    bool invalid_id = false;
    if (type == "savedata")
        invalid_id = !id.starts_with("PCS") || (id.size() != 9);
    else if (type == "trophy")
        invalid_id = !id.starts_with("NPWR") || (id.size() != 12);

    if (invalid_id) {
        log("NPID " + npid + " try to upload with invalid id: " + id);
        res.set_content("ERR:InvalidID", "text/plain");
        return;
    }

    auto msg = "NPID: " + npid + " type: " + type + " id: " + id;

    if (!req.form.has_file("file")) {
        log(msg + ", missing file on upload attempt");
        res.set_content("ERR:MissingFile", "text/plain");
        return;
    }

    const auto file = req.form.get_file("file");
    const uint64_t newSize = file.content.size();

    const fs::path base_path{ fs::path("v3kn") / "Users" / npid / type / id };
    const std::string path = (type == "savedata") ? "savedata.psvimg" : "TROPUSR.DAT";
    const fs::path file_path{ base_path / path };

    uint64_t oldSize = 0;
    if (fs::exists(file_path))
        oldSize = fs::file_size(file_path);

    const int64_t delta = (int64_t)newSize - (int64_t)oldSize;

    uint64_t new_used = 0;
    {
        std::lock_guard<std::mutex> lock_db(account_mutex);
        json db = load_users();
        auto &user = db["users"][npid];

        uint64_t used = user["quota_used"];
        new_used = used + delta;

        if ((delta > 0) && (new_used > DEFAULT_QUOTA_TOTAL)) {
            log(msg + ", exceeded quota on upload attempt. Used: " + std::to_string(used) + ", New Used: " + std::to_string(new_used) + ", Total: " + std::to_string(DEFAULT_QUOTA_TOTAL));
            res.set_content("ERR:QuotaExceeded", "text/plain");
            return;
        }

        user["quota_used"] = new_used;
        user["last_activity"] = std::time(0);
        save_users(db);
    }

    fs::create_directories(base_path);
    {
        std::ofstream out(file_path, std::ios::binary);
        out << file.content;
    }

    if (req.form.has_field("xml")) {
        const auto xml_content = req.form.get_field("xml");
        const fs::path xml_path{ (type == "savedata") ? base_path / "savedata.xml" : base_path.parent_path() / "trophies.xml" };
        std::ofstream xml_out(xml_path, std::ios::binary);
        xml_out << xml_content;
    }

    log(msg + "\nUploaded file " + file_path.string() + " (" + std::to_string(newSize) + " bytes), quota: " + std::to_string(new_used) + " / " + std::to_string(DEFAULT_QUOTA_TOTAL));
    res.set_content("OK:" + std::to_string(new_used) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
}
