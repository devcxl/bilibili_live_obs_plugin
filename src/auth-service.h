#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "bilibili-api.h"
#include "config-manager.h"

using json = nlohmann::json;

struct SessionState {
    std::string room_id;
    std::string csrf;
    std::string uid;
    std::string current_area_id;
    std::vector<std::string> current_area_names;
    bool is_live = false;

    void clear() {
        room_id.clear();
        csrf.clear();
        uid.clear();
        current_area_id.clear();
        current_area_names.clear();
        is_live = false;
    }
};

class UserService {
public:
    UserService(BilibiliApi *api, ConfigManager *cfg, SessionState *state);

    void init_current_user();
    bool has_valid_session() const;
    json save_user_data(const std::string &uid, const json &full_data,
                        const std::string &cookie_str, const std::string &room_id,
                        const std::string &csrf);
    json fetch_full_user_data();
    std::string fetch_room_id(const std::unordered_map<std::string, std::string> &cookies_dict);
    json load_saved_config();
    json refresh_current_user();
    json get_account_list();
    json switch_account(const std::string &uid);
    json logout(const std::string &uid);

private:
    BilibiliApi *api_;
    ConfigManager *cfg_;
    SessionState *state_;

    static json strip_sensitive(const json &data);
};

class LiveService {
public:
    LiveService(BilibiliApi *api, ConfigManager *cfg, SessionState *state);

    json get_partitions();
    json update_title(const std::string &title);
    json update_area(const std::string &p_name, const std::string &s_name);
    json start_live(const std::string &p_name, const std::string &s_name, const std::string &title);
    json stop_live();
    json check_live_status();

    void refresh_partitions();

    // partition data: parent -> {sub_name -> area_id}
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> partition_map_;

private:
    BilibiliApi *api_;
    ConfigManager *cfg_;
    SessionState *state_;

    std::vector<std::string> get_names_by_id(const std::string &area_id);
    std::string get_area_id_for_update(const std::string &p_name, const std::string &s_name);
};

class AuthService {
public:
    AuthService(BilibiliApi *api, UserService *user_svc, LiveService *live_svc, SessionState *state);

    json get_login_qrcode();
    json poll_login_status(const std::string &key);

private:
    BilibiliApi *api_;
    UserService *user_svc_;
    LiveService *live_svc_;
    SessionState *state_;
};
