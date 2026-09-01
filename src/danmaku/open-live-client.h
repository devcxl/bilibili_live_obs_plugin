#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace danmaku {

struct OpenLiveStartResult {
    bool ok = false;
    int code = -1;
    std::string msg;
    std::string game_id;
    std::string auth_body;
    std::vector<std::string> wss_links;
    int64_t room_id = 0;
    std::string anchor_name;
};

class OpenLiveClient {
public:
    // 开启互动项目，获取官方 WebSocket 长链与鉴权 Token
    static OpenLiveStartResult start_app(int64_t app_id,
                                        const std::string &access_key,
                                        const std::string &access_secret,
                                        const std::string &code);

    // 发送项目心跳（每 20 秒调用一次，保活官方会话）
    static bool send_heartbeat(const std::string &game_id,
                               const std::string &access_key,
                               const std::string &access_secret);

    // 关闭互动项目
    static bool end_app(int64_t app_id,
                        const std::string &game_id,
                        const std::string &access_key,
                        const std::string &access_secret);

    // 官方签名计算 helper
    static std::string hmac_sha256_hex(const std::string &key, const std::string &data);
    static std::string md5_hex(const std::string &data);
};

} // namespace danmaku
