// v3knr project
// Copyright (C) 2026 Vita3K team

#include "storage/storage.h"
#include "utils/utils.h"

#include <pugixml.hpp>

#include <fstream>
#include <sstream>

void register_storage_endpoints(httplib::Server &server) {
    server.Get("/v3kn/save_info", handle_get_save_info);
    server.Get("/v3kn/trophies_info", handle_get_trophies_info);
    server.Get("/v3kn/download_file", handle_download_file);
    server.Post("/v3kn/upload_file", handle_upload_file);
    server.Get("/v3kn/check_trophy_conf_data", handle_check_trophy_conf_data);
    server.Post("/v3kn/upload_trophy_conf_data", handle_upload_trophy_conf_data);
    server.Get("/v3kn/check_stitle_info", handle_check_stitle_info);
    server.Post("/v3kn/upload_stitle_info", handle_upload_stitle_info);
}

void handle_get_save_info(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "save info request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    auto titleid = req.get_param_value("titleid");
    if (titleid.empty()) {
        log("Missing TitleID on save info request for online ID " + online_id);
        res.set_content("ERR:MissingTitleID", "text/plain");
        return;
    }

    const std::string savedata_path = "v3kn/Users/" + online_id + "/savedata/" + titleid;
    if (!fs::exists(savedata_path)) {
        log("No savedata for online ID " + online_id + " TitleID " + titleid);
        res.set_content("WARN:NoSavedata", "text/plain");
        return;
    }

    std::ifstream savedata_info_file(fs::path(savedata_path) / "savedata.xml");
    if (!savedata_info_file) {
        log("No savedata info file for online ID " + online_id + " TitleID " + titleid);
        res.set_content("WARN:NoSavedataInfo", "text/plain");
        return;
    }

    std::string savedata_content((std::istreambuf_iterator<char>(savedata_info_file)),
        std::istreambuf_iterator<char>());

    update_last_activity(req, account_id);
    res.set_content(savedata_content, "application/xml");
}

void handle_get_trophies_info(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "trophies info request", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    std::ifstream trophies_info_file(fs::path("v3kn") / "Users" / online_id / "trophy" / "trophies.xml");
    if (!trophies_info_file) {
        log("No trophies info file for online ID " + online_id);
        res.set_content("WARN:NoTrophiesInfo", "text/plain");
        return;
    }

    std::string trophies_content((std::istreambuf_iterator<char>(trophies_info_file)),
        std::istreambuf_iterator<char>());

    update_last_activity(req, account_id);
    res.set_content(trophies_content, "application/xml");
}

void handle_download_file(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "file download", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    const auto type = req.get_param_value("type");
    if ((type != "savedata") && (type != "trophy")) {
        log("online_id " + online_id + " try to download with invalid type: " + type);
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
        log("online_id " + online_id + " try to download with invalid id: " + id);
        res.set_content("ERR:InvalidID", "text/plain");
        return;
    }

    auto msg = "online ID: " + online_id + " type: " + type + " id: " + id;

    const auto path = (type == "savedata") ? "savedata.psvimg" : "TROPUSR.DAT";
    const fs::path file_path{ fs::path("v3kn") / "Users" / online_id / type / id / path };

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

    update_last_activity(req, account_id);
    res.set_content(buffer.str(), "application/octet-stream");
}

static std::mutex trophies_rarity_mutex;
static void update_trophies_rarity(const std::string &online_id, const std::string &npcomm_id) {
    std::lock_guard<std::mutex> lock(trophies_rarity_mutex);

    // Locate the player's trophies.xml
    fs::path trophy_xml_path = fs::path("v3kn") / "Users" / online_id / "trophy" / "trophies.xml";

    if (!fs::exists(trophy_xml_path) || fs::is_empty(trophy_xml_path)) {
        log("rarity: trophies.xml missing for online ID " + online_id);
        return;
    }

    // Load the XML
    pugi::xml_document doc;
    auto result = doc.load_file(trophy_xml_path.string().c_str());
    if (!result) {
        log("rarity: failed to parse trophies.xml for online ID " + online_id + ": " + result.description());
        return;
    }

    // Found the root <trophies>
    pugi::xml_node trophies_root = doc.child("trophies");
    if (!trophies_root) {
        log("rarity: trophies.xml has no <trophies> root for online ID " + online_id);
        return;
    }

    // 3) Load or create trophies_rarity.json
    const fs::path rarity_file{ fs::path("v3kn") / "trophies_rarity.json" };
    json rarity_json;

    if (fs::exists(rarity_file) && !fs::is_empty(rarity_file)) {
        try {
            std::ifstream f(rarity_file);
            f >> rarity_json;
        } catch (...) {
            log("rarity: invalid trophies_rarity.json, recreating");
            rarity_json = json::object();
        }
    }

    // Ensure the two root objects
    if (!rarity_json.contains("players") || !rarity_json["players"].is_object())
        rarity_json["players"] = json::object();

    if (!rarity_json.contains("trophies") || !rarity_json["trophies"].is_object())
        rarity_json["trophies"] = json::object();

    json &players_obj = rarity_json["players"];
    json &trophies_obj = rarity_json["trophies"];

    // Ensure players[npcomm_id] exists and is an array
    if (!players_obj.contains(npcomm_id) || !players_obj[npcomm_id].is_array())
        players_obj[npcomm_id] = json::array();

    auto &players_array = players_obj[npcomm_id];

    if (std::find(players_array.begin(), players_array.end(), online_id) == players_array.end())
        players_array.push_back(online_id);

    // Find the <np commid="NPWRxxxxx_00">
    const auto np_node = trophies_root.find_child_by_attribute("np", "commid", npcomm_id.c_str());
    if (!np_node) {
        log("rarity: no <np> node with commid " + npcomm_id + " for online ID " + online_id);
        return;
    }

    // Ensure trophies[npcomm_id] exists
    if (!trophies_obj.contains(npcomm_id) || !trophies_obj[npcomm_id].is_object())
        trophies_obj[npcomm_id] = json::object();

    auto &trophies_array = trophies_obj[npcomm_id];

    // Read the unlocked trophies
    pugi::xml_node unlocked_node = np_node.child("unlocked");
    if (!unlocked_node) {
        log("rarity: no <unlocked> node for online ID " + online_id + " commid " + npcomm_id);
        return;
    }

    // For each <trophy id="NPWRxxxxx_00"> under <unlocked>
    for (auto trophy_node : unlocked_node.children("trophy")) {
        const std::string trophy_id = trophy_node.attribute("id").as_string();

        // Ensure trophies[npcomm_id][trophy_id] exists and is an array
        if (!trophies_array.contains(trophy_id) || !trophies_array[trophy_id].is_array())
            trophies_array[trophy_id] = json::array();

        // Add the online_id to the trophy's array if not already present
        auto &earned_array = trophies_array[trophy_id];

        // This check is not really needed since a player should only be able to unlock a trophy once, but we do it just in case to avoid duplicates
        if (std::find(earned_array.begin(), earned_array.end(), online_id) == earned_array.end())
            earned_array.push_back(online_id);
    }

    // Save the updated rarity data
    try {
        std::ofstream f(rarity_file);
        f << rarity_json.dump(2);
        log("rarity: updated rarity stats for online ID " + online_id);
    } catch (...) {
        log("rarity: failed to write trophies_rarity.json");
    }
}

void handle_upload_file(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "file upload", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    const auto type = req.get_param_value("type");
    if ((type != "savedata") && (type != "trophy")) {
        log("online ID " + online_id + " try to upload with invalid type: " + type);
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
        log("online ID " + online_id + " try to upload with invalid id: " + id);
        res.set_content("ERR:InvalidID", "text/plain");
        return;
    }

    auto msg = "online ID: " + online_id + " type: " + type + " id: " + id;

    if (!req.form.has_file("file")) {
        log(msg + ", missing file on upload attempt");
        res.set_content("ERR:MissingFile", "text/plain");
        return;
    }

    const auto file = req.form.get_file("file");
    const uint64_t newSize = file.content.size();

    const fs::path base_path{ fs::path("v3kn") / "Users" / online_id / type / id };
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
        auto &user = db["users"][account_id];

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

    if (type == "trophy")
        update_trophies_rarity(online_id, id);

    log(msg + "\nUploaded file " + file_path.string() + " (" + std::to_string(newSize) + " bytes), quota: " + std::to_string(new_used) + " / " + std::to_string(DEFAULT_QUOTA_TOTAL));
    res.set_content("OK:" + std::to_string(new_used) + ":" + std::to_string(DEFAULT_QUOTA_TOTAL), "text/plain");
}

void handle_check_trophy_conf_data(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);
    std::string err;
    const auto account = get_valid_account(req, "trophy conf data check", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string account_id = account->account_id;
    const std::string online_id = account->online_id;

    const fs::path trophies_xml_path{ fs::path("v3kn") / "Users" / online_id / "trophy" / "trophies.xml" };

    pugi::xml_document doc;
    if (!doc.load_file(trophies_xml_path.string().c_str())) {
        log("Failed to load trophies.xml for online ID " + online_id);
        res.set_content("ERR:NoTrophiesInfo", "text/plain");
        return;
    }

    pugi::xml_document response_doc;
    auto response = response_doc.append_child("missing_confs");
    for (pugi::xml_node np_node : doc.child("trophies").children("np")) {
        const std::string commid = np_node.attribute("commid").as_string();

        const auto mark_missing = [&](const std::string &reason) {
            response.append_child("npcommid").text().set(commid.c_str());
            log("online ID " + online_id + " " + reason + " for: " + commid);
        };

        const fs::path conf_path = fs::path("v3kn") / "Trophies" / commid;
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
        if (conf_id != commid) {
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

    update_last_activity(req, account_id);

    if (response.empty()) {
        log("online ID " + online_id + " has all trophy conf data");
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
    const auto account = get_valid_account(req, "trophy conf data upload", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    if (!req.form.has_file("file")) {
        log("online ID " + online_id + " try to upload trophy conf data with missing file");
        res.set_content("ERR:MissingFile", "text/plain");
        return;
    }

    const auto id = req.get_param_value("id");
    if (!id.starts_with("NPWR") || (id.size() != 12)) {
        log("online ID " + online_id + " try to upload trophy conf data with invalid id: " + id);
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

    log("online ID " + online_id + " uploaded trophy conf data for " + id + " " + file.filename + " (" + std::to_string(file.content.size()) + " bytes)");
    res.set_content("OK", "text/plain");
}

void handle_check_stitle_info(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "stitle info check", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    update_last_activity(req, account_id);

    const std::string titleid = req.get_param_value("titleid");
    if (titleid.empty()) {
        log("Missing TitleID on short title info check for online ID " + online_id);
        res.set_content("ERR:MissingTitleID", "text/plain");
        return;
    }

    if (has_stitle_info(titleid)) {
        log("Short Title info exists for TitleID " + titleid + ", online ID " + online_id);
        res.set_content("OK:TitleInfoExists", "text/plain");
        return;
    }

    log("Short Title info missing for TitleID " + titleid + ", online ID " + online_id);
    res.set_content("WARN:TitleInfoMissing", "text/plain");
}

void handle_upload_stitle_info(const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> req_lock(request_mutex);

    std::string err;
    const auto account = get_valid_account(req, "short title info upload", err);
    if (!account) {
        res.set_content(err, "text/plain");
        return;
    }

    const std::string &account_id = account->account_id;
    const std::string &online_id = account->online_id;

    update_last_activity(req, account_id);

    if (req.body.empty()) {
        log("online ID " + online_id + " try to upload short title info with missing payload");
        res.set_content("ERR:MissingPayload", "text/plain");
        return;
    }

    json payload;
    try {
        payload = json::parse(req.body);
    } catch (...) {
        log("online ID " + online_id + " try to upload short title info with invalid JSON");
        res.set_content("ERR:InvalidJson", "text/plain");
        return;
    }

    const std::string titleid = payload.value("titleid", "");
    if (titleid.empty() || !payload.contains("names") || !payload["names"].is_object()) {
        log("online ID " + online_id + " try to upload short title info with invalid payload");
        res.set_content("ERR:InvalidPayload", "text/plain");
        return;
    }

    update_stitle_info(titleid, payload["names"]);

    log("online ID " + online_id + " uploaded short title info for " + titleid);
    res.set_content("OK:ShortTitleInfoSaved", "text/plain");
}
