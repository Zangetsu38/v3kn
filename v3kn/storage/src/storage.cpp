// v3knr project
// Copyright (C) 2026 Vita3K team

#include "storage/storage.h"
#include "utils/utils.h"

#include <pugixml.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

void register_storage_endpoints(httplib::Server &server) {
    server.Get("/v3kn/save_info", handle_get_save_info);
    server.Get("/v3kn/trophies_info", handle_get_trophies_info);
    server.Get("/v3kn/download_file", handle_download_file);
    server.Post("/v3kn/upload_file", handle_upload_file);
    server.Get("/v3kn/check_trophy_conf_data", handle_check_trophy_conf_data);
    server.Post("/v3kn/upload_trophy_conf_data", handle_upload_trophy_conf_data);
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

void handle_check_trophy_conf_data(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const std::string npid = get_valid_npid(req, "trophy conf data check", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    const fs::path trophies_xml_path{ fs::path("v3kn") / "Users" / npid / "trophy" / "trophies.xml" };

    pugi::xml_document doc;
    if (!doc.load_file(trophies_xml_path.string().c_str())) {
        log("Failed to load trophies.xml for NPID " + npid);
        res.set_content("ERR:NoTrophiesInfo", "text/plain");
        return;
    }

    pugi::xml_document response_doc;
    auto response = response_doc.append_child("missing_confs");
    for (pugi::xml_node trophy_node : doc.child("trophies").children("trophy")) {
        const std::string id = trophy_node.attribute("id").as_string();

        const auto mark_missing = [&](const std::string &reason) {
            response.append_child("trophy").append_attribute("id").set_value(id.c_str());
            log("NPID " + npid + " " + reason + " for: " + id);
        };

        const fs::path conf_path = fs::path("v3kn") / "Trophies" / id;
        if (!fs::exists(conf_path) || fs::is_empty(conf_path)) {
            mark_missing("is missing trophy conf data");
            continue;
        }

        const fs::path tropconf_path = conf_path / "TROPCONF.SFM";
        if (!fs::exists(tropconf_path) || !fs::exists(conf_path / "TROP.SFM")) {
            mark_missing("is incomplete trophy conf data");
            continue;
        }

        if (!fs::exists(conf_path / "ICON0.PNG")) {
            mark_missing("is missing icon for trophy conf data");
            continue;
        }

        pugi::xml_document conf_doc;
        if (!conf_doc.load_file(tropconf_path.string().c_str())) {
            mark_missing("failed to load TROPCONF.SFM");
            continue;
        }

        auto trophyconf = conf_doc.child("trophyconf");
        if (!trophyconf) {
            mark_missing("missing trophy node in TROPCONF.SFM");
            continue;
        }
        const std::string conf_id = trophyconf.child("npcommid").text().as_string();
        if (conf_id != id) {
            mark_missing("trophy id mismatch in TROPCONF.SFM, found: " + conf_id);
            continue;
        }

        for (pugi::xml_node trophy : trophyconf.children("trophy")) {
            const auto trophy_id = trophy.attribute("id");
            if (trophy_id.empty()) {
                mark_missing("missing trophy id in TROPCONF.SFM");
                break;
            }

            const std::string trophy_id_str = trophy_id.as_string();
            const fs::path trophy_icon_path = conf_path / ("TROP" + trophy_id_str + ".PNG");
            if (!fs::exists(trophy_icon_path)) {
                mark_missing("missing icon for trophy id: " + trophy_id_str);
                break;
            }
        }
    }

    update_last_activity(req, npid);

    if (response.empty()) {
        log("NPID " + npid + " has all trophy conf data");
        res.set_content("OK", "text/plain");
    } else {
        std::stringstream ss;
        response_doc.save(ss);
        std::string missing_confs((std::istreambuf_iterator<char>(ss)),
            std::istreambuf_iterator<char>());

        res.set_content(missing_confs, "application/xml");
    }
}

void handle_upload_trophy_conf_data(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const std::string npid = get_valid_npid(req, "trophy conf data upload", err);
    if (npid.empty()) {
        res.set_content(err, "text/plain");
        return;
    }

    if (!req.form.has_file("file")) {
        log("NPID " + npid + " try to upload trophy conf data with missing file");
        res.set_content("ERR:MissingFile", "text/plain");
        return;
    }

    const auto id = req.get_param_value("id");
    if (!id.starts_with("NPWR") || (id.size() != 12)) {
        log("NPID " + npid + " try to upload trophy conf data with invalid id: " + id);
        res.set_content("ERR:InvalidID", "text/plain");
        return;
    }

    const auto file = req.form.get_file("file");
    const fs::path base_path{ fs::path("v3kn") / "Trophies" / id };
    const fs::path file_path{ base_path / file.filename };

    fs::create_directories(base_path);
    {
        std::ofstream out(file_path, std::ios::binary);
        out << file.content;
    }

    log("NPID " + npid + " uploaded trophy conf data for " + id + " " + file.filename + " (" + std::to_string(file.content.size()) + " bytes)");
    res.set_content("OK", "text/plain");
}
