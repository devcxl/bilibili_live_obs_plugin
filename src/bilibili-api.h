#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

using json = nlohmann::json;

struct ApiResult {
    bool ok = false;
    json data;
    int code = -1;
    std::string msg;
    std::unordered_map<std::string, std::string> response_cookies;
};

class BilibiliApi {
public:
    BilibiliApi();
    ~BilibiliApi();

    void update_cookies(const std::unordered_map<std::string, std::string> &cookies);
    void set_csrf(const std::string &csrf) {
        std::lock_guard<std::mutex> lock(api_mutex_);
        csrf_ = csrf;
    }

    // 从当前 cookie 串中取指定键的值（不存在返回空串）
    std::string cookie_value(const std::string &name) const;
    // 登录用户 UID（cookie 中的 DedeUserID），未登录返回 0
    int64_t cookie_uid() const;

    // Auth
    ApiResult get_passport_qrcode();
    ApiResult poll_passport_qrcode(const std::string &qrcode_key);

    // User
    ApiResult get_user_info();
    ApiResult get_user_stat();

    // Room
    ApiResult get_room_id_by_uid(const std::string &uid);
    ApiResult get_room_info(const std::string &room_id);
    ApiResult get_area_list();
    ApiResult update_title(const std::string &room_id, const std::string &title);
    ApiResult update_area(const std::string &room_id, const std::string &area_id);

    // Live
    ApiResult start_live(const std::string &room_id, const std::string &area_id);
    ApiResult stop_live(const std::string &room_id);

    // Misc
    ApiResult get_server_time();
    // 注销 B 站会话（使 SESSDATA 服务端失效），需携带当前登录 cookie + csrf
    ApiResult logout_session();

    bool get_wbi_keys(std::string &img_key, std::string &sub_key);
    static std::string sign_wbi(json params, const std::string &img_key, const std::string &sub_key);

private:
    // 线程安全：独立 curl 句柄，mutex 仅保护 cookie_str_ 与 csrf_ 读写
    mutable std::mutex api_mutex_;
    std::string cookie_str_;
    std::string csrf_;

    static const std::string USER_AGENT;
    static const std::string APP_KEY;
    static const std::string APP_SEC;
    static const int MIXIN_KEY_ENC_TAB[64];

    ApiResult do_get(const std::string &url, const json &params = nullptr);
    ApiResult do_post(const std::string &url, const json &data = nullptr, const json &params = nullptr);
    ApiResult do_request(const std::string &method, const std::string &url,
                         const json &params, const json &data);

    json appsign(json params) const;
    std::string build_query(const json &params) const;

    static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata);
    static size_t header_cb(void *ptr, size_t size, size_t nmemb, void *userdata);
};
