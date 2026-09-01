#pragma once

#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct UserData {
    std::string uid;
    std::string uname;
    std::string face;
    std::string cookie;
    std::string roomId;
    std::string csrf;
    int level = 0;
    int current_exp = 0;
    int next_exp = 0;
    int money = 0;
    int bcoin = 0;
    int following = 0;
    int follower = 0;
    int dynamic_count = 0;
    std::string last_title;
    std::string last_area_id;
    std::vector<std::string> last_area_name;

    json to_json() const;
    static UserData from_json(const json &j);
};

struct TtsConfigData {
    bool enabled = false;
    std::string key;
    std::string region = "eastasia";
    std::string voice = "zh-CN-XiaoxiaoNeural";
    std::string rate = "+0%";
    std::string pitch = "+0%";
    int volume = 100;
    bool read_danmaku = true;
    bool read_gift = true;
    bool read_sc = true;
    bool read_entry = false;             // 是否开启进房播报
    int entry_filter = 0;                // 进房播报范围: 0=全部观众, 1=仅粉丝勋章, 2=仅大航海(舰长/提督/总督)
    bool merge_enabled = true;
};

class ConfigManager {
public:
    ConfigManager();

    void load();
    void save();

    std::unordered_map<std::string, UserData> users;
    std::string current_uid;
    TtsConfigData tts;

    static std::string config_dir();
    static std::string config_path();

private:
    std::string key_;
    std::string key_path();

    std::string load_or_create_key();
    std::string encrypt(const std::string &plain) const;
    std::string decrypt(const std::string &cipher) const;

    void migrate_legacy(const json &legacy);
};
