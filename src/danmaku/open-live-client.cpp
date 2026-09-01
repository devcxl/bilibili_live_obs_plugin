#include "open-live-client.h"

#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#include <curl/curl.h>
#include <obs-module.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <map>

namespace danmaku {

static size_t curl_write_string(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    auto *mem = static_cast<std::string *>(userp);
    mem->append(static_cast<char *>(contents), realsize);
    return realsize;
}

std::string OpenLiveClient::md5_hex(const std::string &data)
{
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    std::ostringstream oss;
    for (unsigned char b : digest) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

std::string OpenLiveClient::hmac_sha256_hex(const std::string &key, const std::string &data)
{
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &result_len);

    std::ostringstream oss;
    for (unsigned int i = 0; i < result_len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(result[i]);
    }
    return oss.str();
}

static std::string generate_nonce()
{
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(now);
    std::uniform_int_distribution<uint64_t> dist;
    return std::to_string(dist(rng));
}

static std::pair<int, std::string> do_signed_post(const std::string &url,
                                                   const std::string &json_body,
                                                   const std::string &access_key,
                                                   const std::string &access_secret)
{
    std::string timestamp = std::to_string(std::time(nullptr));
    std::string nonce = generate_nonce();
    std::string content_md5 = OpenLiveClient::md5_hex(json_body);

    std::map<std::string, std::string> bili_headers = {
        {"x-bili-accesskeyid", access_key},
        {"x-bili-content-md5", content_md5},
        {"x-bili-signature-method", "HMAC-SHA256"},
        {"x-bili-signature-nonce", nonce},
        {"x-bili-signature-version", "1.0"},
        {"x-bili-timestamp", timestamp}
    };

    std::string string_to_sign;
    for (const auto &[k, v] : bili_headers) {
        string_to_sign += k + ":" + v + "\n";
    }
    // 移除最后一个换行符以匹配规范
    if (!string_to_sign.empty() && string_to_sign.back() == '\n') {
        string_to_sign.pop_back();
    }

    std::string signature = OpenLiveClient::hmac_sha256_hex(access_secret, string_to_sign);

    CURL *curl = curl_easy_init();
    if (!curl) return {-1, "curl_easy_init failed"};

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    for (const auto &[k, v] : bili_headers) {
        std::string h = k + ": " + v;
        headers = curl_slist_append(headers, h.c_str());
    }
    std::string auth_header = "Authorization: " + signature;
    headers = curl_slist_append(headers, auth_header.c_str());

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode cc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (cc != CURLE_OK) {
        return {-1, curl_easy_strerror(cc)};
    }
    return {0, response_body};
}

OpenLiveStartResult OpenLiveClient::start_app(int64_t app_id,
                                              const std::string &access_key,
                                              const std::string &access_secret,
                                              const std::string &code)
{
    OpenLiveStartResult res;
    if (app_id <= 0 || access_key.empty() || access_secret.empty() || code.empty()) {
        res.msg = "参数不完整（请在设置中填写 App ID, AccessKey, Secret 与主播身份码）";
        return res;
    }

    nlohmann::json req_body = {
        {"code", code},
        {"app_id", app_id}
    };
    std::string body_str = req_body.dump();

    auto [code_err, resp_str] = do_signed_post(
        "https://live-open.bilibili.com/v2/app/start",
        body_str, access_key, access_secret
    );

    if (code_err != 0) {
        res.msg = resp_str;
        return res;
    }

    try {
        auto j = nlohmann::json::parse(resp_str);
        res.code = j.value("code", -1);
        res.msg = j.value("message", j.value("msg", ""));

        if (res.code == 0 && j.contains("data") && j["data"].is_object()) {
            const auto &d = j["data"];
            res.ok = true;
            if (d.contains("game_info") && d["game_info"].is_object()) {
                res.game_id = d["game_info"].value("game_id", "");
            }
            if (d.contains("anchor_info") && d["anchor_info"].is_object()) {
                res.room_id = d["anchor_info"].value("room_id", 0LL);
                res.anchor_name = d["anchor_info"].value("uname", "");
            }
            if (d.contains("websocket_info") && d["websocket_info"].is_object()) {
                const auto &ws_info = d["websocket_info"];
                res.auth_body = ws_info.value("auth_body", "");
                if (ws_info.contains("wss_link") && ws_info["wss_link"].is_array()) {
                    for (const auto &link : ws_info["wss_link"]) {
                        if (link.is_string()) res.wss_links.push_back(link.get<std::string>());
                    }
                }
            }
        }
    } catch (...) {
        res.msg = "解析官方开放平台响应 JSON 失败";
    }

    return res;
}

bool OpenLiveClient::send_heartbeat(const std::string &game_id,
                                    const std::string &access_key,
                                    const std::string &access_secret)
{
    if (game_id.empty()) return false;

    nlohmann::json req_body = {
        {"game_id", game_id}
    };
    auto [code_err, resp_str] = do_signed_post(
        "https://live-open.bilibili.com/v2/app/heartbeat",
        req_body.dump(), access_key, access_secret
    );
    return (code_err == 0);
}

bool OpenLiveClient::end_app(int64_t app_id,
                             const std::string &game_id,
                             const std::string &access_key,
                             const std::string &access_secret)
{
    if (game_id.empty() || app_id <= 0) return false;

    nlohmann::json req_body = {
        {"app_id", app_id},
        {"game_id", game_id}
    };
    auto [code_err, resp_str] = do_signed_post(
        "https://live-open.bilibili.com/v2/app/end",
        req_body.dump(), access_key, access_secret
    );
    return (code_err == 0);
}

} // namespace danmaku
