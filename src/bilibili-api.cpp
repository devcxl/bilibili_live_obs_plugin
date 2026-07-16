#include "bilibili-api.h"

#include <algorithm>
#include <cstring>
#include <regex>
#include <sstream>
#include <iomanip>
#include <openssl/md5.h>

// ── static constants ──

#define BILI_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" \
    " AppleWebKit/537.36 (KHTML, like Gecko)" \
    " Chrome/120.0.0.0 Safari/537.36"

const std::string BilibiliApi::USER_AGENT = BILI_USER_AGENT;

const std::string BilibiliApi::APP_KEY = "aae92bc66f3edfab";
const std::string BilibiliApi::APP_SEC = "af125a0d5279fd576c1b4418a3e8276d";

const int BilibiliApi::MIXIN_KEY_ENC_TAB[64] = {
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
    27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30,
    4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52
};

// ── helpers ──

static std::string md5_hex(const std::string &in)
{
    unsigned char hash[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char *>(in.data()), in.size(), hash);
    std::ostringstream out;
    for (auto c : hash)
        out << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return out.str();
}

static std::string url_encode(const std::string &s)
{
    std::ostringstream out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << c;
        else
            out << '%' << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    }
    return out.str();
}

static std::string ck_str_to_dict(const std::string &ck)
{
    return ck; // stored as-is, we use cookie string directly
}

static json parse_qs(const std::string &qs)
{
    json j;
    std::istringstream stream(qs);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = pair.substr(0, eq);
            std::string v = pair.substr(eq + 1);
            // simple URL decode
            std::string dec;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '%' && i + 2 < v.size()) {
                    int c;
                    std::istringstream(v.substr(i + 1, 2)) >> std::hex >> c;
                    dec += (char)c;
                    i += 2;
                } else if (v[i] == '+') {
                    dec += ' ';
                } else {
                    dec += v[i];
                }
            }
            j[k] = dec;
        }
    }
    return j;
}

// ── constructor / destructor ──

BilibiliApi::BilibiliApi()
{
    curl_ = curl_easy_init();
}

BilibiliApi::~BilibiliApi()
{
    if (curl_) curl_easy_cleanup(curl_);
}

void BilibiliApi::update_cookies(const std::unordered_map<std::string, std::string> &cookies)
{
    cookie_str_.clear();
    for (auto &[k, v] : cookies) {
        if (!cookie_str_.empty()) cookie_str_ += "; ";
        cookie_str_ += k + "=" + v;
        if (k == "bili_jct") csrf_ = v;
    }
}

// ── curl callbacks ──

size_t BilibiliApi::write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *str = static_cast<std::string *>(userdata);
    str->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

size_t BilibiliApi::header_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *cookies = static_cast<std::unordered_map<std::string, std::string> *>(userdata);
    std::string header(static_cast<char *>(ptr), size * nmemb);
    static const std::regex set_cookie_re("^set-cookie:\\s*([^=]+)=([^;]+)", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(header, m, set_cookie_re)) {
        (*cookies)[m[1]] = m[2];
    }
    return size * nmemb;
}

// ── request core ──

std::string BilibiliApi::build_query(const json &params) const
{
    if (params.is_null()) return "";
    std::string qs;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!qs.empty()) qs += "&";
        qs += url_encode(it.key()) + "=" + url_encode(it.value().dump());
    }
    return qs;
}

json BilibiliApi::appsign(json params) const
{
    params["appkey"] = APP_KEY;
    // sort keys
    std::vector<std::string> keys;
    for (auto it = params.begin(); it != params.end(); ++it)
        keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());

    std::string qs;
    for (auto &k : keys) {
        if (!qs.empty()) qs += "&";
        qs += k + "=" + url_encode(params[k].dump());
    }
    params["sign"] = md5_hex(qs + APP_SEC);
    return params;
}

std::string BilibiliApi::mask_url(const std::string &url) const
{
    std::string result = url;
    auto qpos = result.find('?');
    if (qpos == std::string::npos) return result;

    auto fragment = result.substr(qpos + 1);
    auto params = parse_qs(fragment);
    static const char *sensitive[] = {"uid", "room_id", "key", "token", "csrf", "csrf_token", "access_key", "qrcode_key"};
    bool changed = false;
    for (auto name : sensitive) {
        if (params.contains(name)) {
            auto v = params[name].get<std::string>();
            if (v.size() > 4)
                params[name] = v.substr(0, 2) + "*****" + v.substr(v.size() - 2);
            changed = true;
        }
    }
    if (!changed) return result;
    return result.substr(0, qpos + 1) + build_query(params);
}

ApiResult BilibiliApi::do_request(const std::string &method, const std::string &url,
                                   const json &params, const json &data)
{
    ApiResult res;
    if (!curl_) {
        res.msg = "curl not initialized";
        return res;
    }

    std::string response_body;
    std::unordered_map<std::string, std::string> resp_cookies;

    // Build URL with params
    std::string full_url = url;
    if (!params.is_null()) {
        full_url += "?" + build_query(params);
    }

    curl_easy_setopt(curl_, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &resp_cookies);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);

    // Headers
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "accept: application/json, text/plain, */*");
    headers = curl_slist_append(headers, "accept-language: zh-CN,zh;q=0.9");
    headers = curl_slist_append(headers, "user-agent: " BILI_USER_AGENT);
    headers = curl_slist_append(headers, "origin: https://www.bilibili.com");
    headers = curl_slist_append(headers, "referer: https://www.bilibili.com/");

    if (method == "POST") {
        std::string postdata;
        if (!data.is_null()) {
            postdata = build_query(data);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, postdata.c_str());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)postdata.size());
        } else {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, "");
        }
        curl_slist_append(headers, "content-type: application/x-www-form-urlencoded; charset=UTF-8");
    }

    // Cookies
    if (!cookie_str_.empty()) {
        curl_easy_setopt(curl_, CURLOPT_COOKIE, cookie_str_.c_str());
    }

    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    CURLcode cc = curl_easy_perform(curl_);
    curl_slist_free_all(headers);

    if (cc != CURLE_OK) {
        res.msg = curl_easy_strerror(cc);
        return res;
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    try {
        auto j = json::parse(response_body);
        res.ok = true;
        res.data = j;
        res.code = j.value("code", -1);
        res.msg = j.value("msg", j.value("message", ""));
        res.response_cookies = resp_cookies;
    } catch (...) {
        res.msg = "JSON decode error";
    }

    return res;
}

ApiResult BilibiliApi::do_get(const std::string &url, const json &params)
{
    return do_request("GET", url, params, nullptr);
}

ApiResult BilibiliApi::do_post(const std::string &url, const json &data, const json &params)
{
    return do_request("POST", url, params, data);
}

// ── WBI signing ──

bool BilibiliApi::get_wbi_keys(std::string &img_key, std::string &sub_key)
{
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.bilibili.com/x/web-interface/nav");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct curl_slist *h = nullptr;
    h = curl_slist_append(h, "user-agent: " BILI_USER_AGENT);
    h = curl_slist_append(h, "referer: https://www.bilibili.com/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);

    CURLcode cc = curl_easy_perform(curl);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);

    if (cc != CURLE_OK) return false;

    try {
        auto j = json::parse(body);
        if (j.value("code", -1) != 0) return false;
        auto img_url = j["data"]["wbi_img"]["img_url"].get<std::string>();
        auto sub_url = j["data"]["wbi_img"]["sub_url"].get<std::string>();
        img_key = img_url.substr(img_url.rfind('/') + 1);
        sub_key = sub_url.substr(sub_url.rfind('/') + 1);
        img_key = img_key.substr(0, img_key.find('.'));
        sub_key = sub_key.substr(0, sub_key.find('.'));
        return true;
    } catch (...) {
        return false;
    }
}

std::string BilibiliApi::sign_wbi(json params, const std::string &img_key, const std::string &sub_key)
{
    // mixin key
    std::string mixin = img_key + sub_key;
    std::string mixed;
    for (int i = 0; i < 32 && i < 64; i++)
        mixed += mixin[MIXIN_KEY_ENC_TAB[i] % mixin.size()];

    // wts
    params["wts"] = std::to_string(std::time(nullptr));

    // sort keys
    std::vector<std::string> keys;
    for (auto it = params.begin(); it != params.end(); ++it)
        keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());

    std::string qs;
    for (auto &k : keys) {
        auto v = params[k].dump();
        // strip quotes
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        // filter !'()*
        std::string filtered;
        for (char c : v)
            if (c != '!' && c != '\'' && c != '(' && c != ')' && c != '*')
                filtered += c;
        if (!qs.empty()) qs += "&";
        qs += k + "=" + url_encode(filtered);
    }

    params["w_rid"] = md5_hex(qs + mixed);
    return qs + "&w_rid=" + params["w_rid"].get<std::string>() + "&wts=" + params["wts"].get<std::string>();
}

// ── API methods ──

ApiResult BilibiliApi::get_passport_qrcode()
{
    return do_get("https://passport.bilibili.com/x/passport-login/web/qrcode/generate");
}

ApiResult BilibiliApi::poll_passport_qrcode(const std::string &qrcode_key)
{
    // Need a separate call that also returns cookies
    CURL *curl = curl_easy_init();
    std::string body;
    std::unordered_map<std::string, std::string> resp_cookies;

    std::string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" + url_encode(qrcode_key);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_cookies);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct curl_slist *h = nullptr;
    h = curl_slist_append(h, "user-agent: " BILI_USER_AGENT);
    h = curl_slist_append(h, "referer: https://www.bilibili.com/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);

    ApiResult res;
    CURLcode cc = curl_easy_perform(curl);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);

    if (cc != CURLE_OK) {
        res.msg = curl_easy_strerror(cc);
        return res;
    }

    try {
        auto j = json::parse(body);
        res.ok = true;
        res.data = j;
        res.response_cookies = resp_cookies;
    } catch (...) {
        res.msg = "JSON decode error";
    }
    return res;
}

ApiResult BilibiliApi::get_user_info()
{
    return do_get("https://api.bilibili.com/x/web-interface/nav");
}

ApiResult BilibiliApi::get_user_stat()
{
    return do_get("https://api.bilibili.com/x/web-interface/nav/stat");
}

ApiResult BilibiliApi::get_room_id_by_uid(const std::string &uid)
{
    return do_get("https://api.live.bilibili.com/room/v2/Room/room_id_by_uid",
                   json{{"uid", uid}});
}

ApiResult BilibiliApi::get_room_info(const std::string &room_id)
{
    return do_get("https://api.live.bilibili.com/room/v1/Room/get_info",
                   json{{"room_id", room_id}});
}

ApiResult BilibiliApi::get_area_list()
{
    return do_get("https://api.live.bilibili.com/room/v1/Area/getList",
                   json{{"show_pinyin", 1}});
}

ApiResult BilibiliApi::update_title(const std::string &room_id, const std::string &title)
{
    return do_post("https://api.live.bilibili.com/room/v1/Room/update",
                    json{{"room_id", room_id}, {"platform", "pc_link"},
                          {"title", title}, {"csrf_token", csrf_}, {"csrf", csrf_}});
}

ApiResult BilibiliApi::update_area(const std::string &room_id, const std::string &area_id)
{
    return do_post("https://api.live.bilibili.com/room/v1/Room/update",
                    json{{"room_id", room_id}, {"area_id", area_id},
                          {"platform", "pc_link"},
                          {"csrf_token", csrf_}, {"csrf", csrf_}});
}

ApiResult BilibiliApi::start_live(const std::string &room_id, const std::string &area_id)
{
    auto ts_res = do_get("https://api.bilibili.com/x/report/click/now");
    if (!ts_res.ok || ts_res.code != 0)
        return ts_res;

    std::string ts = std::to_string(ts_res.data["data"]["now"].get<int64_t>());

    auto v_params = appsign(json{{"system_version", 2}, {"ts", ts}});
    auto v_res = do_get("https://api.live.bilibili.com/xlive/app-blink/v1/liveVersionInfo/getHomePageLiveVersion",
                         v_params);
    if (!v_res.ok || v_res.code != 0)
        return v_res;

    json post_data = appsign(json{
        {"room_id", room_id}, {"platform", "pc_link"},
        {"area_v2", area_id}, {"backup_stream", "0"},
        {"csrf_token", csrf_}, {"csrf", csrf_},
        {"build", v_res.data["data"]["build"]},
        {"version", v_res.data["data"]["curr_version"]},
        {"ts", ts}
    });
    return do_post("https://api.live.bilibili.com/room/v1/Room/startLive", post_data);
}

ApiResult BilibiliApi::stop_live(const std::string &room_id)
{
    return do_post("https://api.live.bilibili.com/room/v1/Room/stopLive",
                    json{{"room_id", room_id}, {"platform", "pc_link"},
                          {"csrf_token", csrf_}, {"csrf", csrf_}});
}

ApiResult BilibiliApi::get_server_time()
{
    return do_get("https://api.bilibili.com/x/report/click/now");
}

ApiResult BilibiliApi::get_buvid3()
{
    return do_get("https://api.bilibili.com/x/frontend/finger/spi");
}
