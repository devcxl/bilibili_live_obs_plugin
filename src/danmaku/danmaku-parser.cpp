#include "danmaku-parser.h"
#include <obs-module.h>

namespace danmaku {

// ── 类型安全的 JSON 提取 Helper ──

static std::string safe_str(const nlohmann::json &j, const std::string &key, const std::string &def = "")
{
    if (!j.contains(key)) return def;
    const auto &v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    return def;
}

static int64_t safe_int64(const nlohmann::json &j, const std::string &key, int64_t def = 0)
{
    if (!j.contains(key)) return def;
    const auto &v = j[key];
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_unsigned()) return static_cast<int64_t>(v.get<uint64_t>());
    if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
    if (v.is_string()) {
        try { return std::stoll(v.get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

static int safe_int(const nlohmann::json &j, const std::string &key, int def = 0)
{
    return static_cast<int>(safe_int64(j, key, def));
}

static double safe_double(const nlohmann::json &j, const std::string &key, double def = 0.0)
{
    if (!j.contains(key)) return def;
    const auto &v = j[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try { return std::stod(v.get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

static bool safe_bool(const nlohmann::json &j, const std::string &key, bool def = false)
{
    if (!j.contains(key)) return def;
    const auto &v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return (v.get<int>() != 0);
    return def;
}

std::optional<ParsedEvent> DanmakuParser::parse(const std::string &json_str)
{
    try {
        auto j = nlohmann::json::parse(json_str, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;

        std::string cmd = safe_str(j, "cmd");
        if (cmd.empty()) return std::nullopt;

        // ── 官方开放平台标准命令 (LIVE_OPEN_PLATFORM_*) ──
        if (cmd == cmd::DM) {
            return parse_open_platform_dm(j);
        }
        if (cmd == cmd::SEND_GIFT) {
            return parse_open_platform_gift(j);
        }
        if (cmd == cmd::SUPER_CHAT) {
            return parse_open_platform_super_chat(j);
        }
        if (cmd == cmd::GUARD) {
            return parse_open_platform_guard(j);
        }
        if (cmd == cmd::LIKE) {
            return parse_open_platform_like(j);
        }
        if (cmd == cmd::ENTER_ROOM || cmd == "LIVE_OPEN_PLATFORM_LIVE_ROOM_ENTER" ||
            cmd == "LIVE_OPEN_PLATFORM_ENTER_ROOM" || cmd == "INTERACT_WORD" ||
            cmd == "LIVE_OPEN_PLATFORM_INTERACT_WORD" ||
            cmd == "ENTRY_EFFECT" || cmd == "WELCOME" || cmd == "WELCOME_GUARD") {
            return parse_open_platform_enter_room(j);
        }
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[danmaku-parser] 解析异常拦截: %s", e.what());
    } catch (...) {
        blog(LOG_WARNING, "[danmaku-parser] 未知解析异常拦截");
    }

    return std::nullopt;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_dm(const nlohmann::json &j)
{
    try {
        if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
        const auto &d = j["data"];

        ParsedEvent ev;
        ev.type = EventType::Danmaku;
        ev.danmaku.cmd = cmd::DM;
        ev.danmaku.username = safe_str(d, "uname");
        ev.danmaku.message = safe_str(d, "msg");
        ev.danmaku.uid = safe_str(d, "open_id");
        ev.danmaku.fan_badge = safe_str(d, "fans_medal_name");
        ev.danmaku.fan_badge_level = safe_int(d, "fans_medal_level");
        ev.danmaku.guard_level = safe_int(d, "guard_level");
        ev.danmaku.is_admin = (safe_int(d, "is_admin") == 1);
        ev.danmaku.dm_type = safe_int(d, "dm_type");
        ev.danmaku.emoji_url = safe_str(d, "emoji_img_url");

        if (ev.danmaku.username.empty() || ev.danmaku.message.empty()) {
            return std::nullopt;
        }
        return ev;
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[danmaku-parser] parse_open_platform_dm error: %s", e.what());
        return std::nullopt;
    }
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_gift(const nlohmann::json &j)
{
    try {
        if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
        const auto &d = j["data"];

        ParsedEvent ev;
        ev.type = EventType::Gift;
        ev.gift.username = safe_str(d, "uname");
        ev.gift.uid = safe_str(d, "open_id");
        ev.gift.gift_name = safe_str(d, "gift_name");
        ev.gift.num = safe_int(d, "gift_num", 1);
        ev.gift.price = safe_int64(d, "price", 0LL);
        ev.gift.paid = safe_bool(d, "paid", true);
        ev.gift.guard_level = safe_int(d, "guard_level");
        ev.gift.medal_name = safe_str(d, "fans_medal_name");
        ev.gift.medal_level = safe_int(d, "fans_medal_level");
        ev.gift.action = "赠送";

        if (d.contains("combo_info") && d["combo_info"].is_object()) {
            ev.gift.combo_num = safe_int(d["combo_info"], "combo_num", 0);
        } else {
            ev.gift.combo_num = safe_int(d, "combo_send", 0);
        }

        if (ev.gift.username.empty() || ev.gift.gift_name.empty()) {
            return std::nullopt;
        }
        return ev;
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[danmaku-parser] parse_open_platform_gift error: %s", e.what());
        return std::nullopt;
    }
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_super_chat(const nlohmann::json &j)
{
    try {
        if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
        const auto &d = j["data"];

        ParsedEvent ev;
        ev.type = EventType::SuperChat;
        ev.super_chat.message_id = safe_int64(d, "message_id");
        ev.super_chat.username = safe_str(d, "uname");
        ev.super_chat.uid = safe_str(d, "open_id");
        ev.super_chat.message = safe_str(d, "message");
        ev.super_chat.guard_level = safe_int(d, "guard_level");
        ev.super_chat.medal_name = safe_str(d, "fans_medal_name");
        ev.super_chat.medal_level = safe_int(d, "fans_medal_level");

        double rmb = safe_double(d, "rmb", 0.0);
        ev.super_chat.price = static_cast<int>(rmb * 100);

        if (ev.super_chat.username.empty() || ev.super_chat.message.empty()) {
            return std::nullopt;
        }
        return ev;
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[danmaku-parser] parse_open_platform_super_chat error: %s", e.what());
        return std::nullopt;
    }
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_guard(const nlohmann::json &j)
{
    try {
        if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
        const auto &d = j["data"];

        ParsedEvent ev;
        ev.type = EventType::Guard;

        std::string uname = safe_str(d, "uname");
        if (uname.empty() && d.contains("user_info") && d["user_info"].is_object()) {
            uname = safe_str(d["user_info"], "uname");
            ev.guard.uid = safe_str(d["user_info"], "open_id");
        } else {
            ev.guard.uid = safe_str(d, "open_id");
        }
        ev.guard.username = uname;
        ev.guard.guard_level = safe_int(d, "guard_level", 3);
        ev.guard.guard_num = safe_int(d, "guard_num", 1);
        ev.guard.guard_unit = safe_str(d, "guard_unit", "月");
        ev.guard.medal_name = safe_str(d, "fans_medal_name");
        ev.guard.medal_level = safe_int(d, "fans_medal_level");
        ev.guard.timestamp = safe_int64(d, "timestamp");

        if (ev.guard.username.empty()) return std::nullopt;
        return ev;
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[danmaku-parser] parse_open_platform_guard error: %s", e.what());
        return std::nullopt;
    }
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_like(const nlohmann::json &j)
{
    try {
        if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
        const auto &d = j["data"];

        ParsedEvent ev;
        ev.type = EventType::Like;
        ev.like.username = safe_str(d, "uname");
        ev.like.uid = safe_str(d, "open_id");
        ev.like.like_text = safe_str(d, "like_text", "点赞了");
        ev.like.like_count = safe_int64(d, "like_count", 1LL);
        ev.like.medal_name = safe_str(d, "fans_medal_name");
        ev.like.medal_level = safe_int(d, "fans_medal_level");
        ev.like.timestamp = safe_int64(d, "timestamp");

        if (ev.like.username.empty()) return std::nullopt;
        return ev;
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[danmaku-parser] parse_open_platform_like error: %s", e.what());
        return std::nullopt;
    }
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_enter_room(const nlohmann::json &j)
{
    try {
        if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
        const auto &d = j["data"];

        ParsedEvent ev;
        ev.type = EventType::Entry;

        std::string uname = safe_str(d, "uname");
        if (uname.empty()) uname = safe_str(d, "nickname");
        if (uname.empty()) uname = safe_str(d, "username");

        if (uname.empty() && d.contains("user_info") && d["user_info"].is_object()) {
            uname = safe_str(d["user_info"], "uname");
        }
        if (uname.empty() && d.contains("copy_writing")) {
            std::string cw = safe_str(d, "copy_writing");
            auto start = cw.find("<%");
            auto end = cw.find("%>");
            if (start != std::string::npos && end != std::string::npos && end > start + 2) {
                uname = cw.substr(start + 2, end - start - 2);
            }
        }

        ev.entry.username = uname;
        ev.entry.uid = safe_str(d, "open_id");
        if (ev.entry.uid.empty()) {
            ev.entry.uid = safe_str(d, "uid");
        }

        ev.entry.guard_level = safe_int(d, "guard_level");
        if (ev.entry.guard_level == 0) {
            ev.entry.guard_level = safe_int(d, "privilege_type");
        }

        ev.entry.medal_name = safe_str(d, "fans_medal_name");
        ev.entry.medal_level = safe_int(d, "fans_medal_level");

        if (d.contains("fans_medal") && d["fans_medal"].is_object()) {
            const auto &fm = d["fans_medal"];
            if (ev.entry.medal_name.empty()) ev.entry.medal_name = safe_str(fm, "medal_name");
            if (ev.entry.medal_level == 0) ev.entry.medal_level = safe_int(fm, "medal_level");
            if (ev.entry.guard_level == 0) ev.entry.guard_level = safe_int(fm, "guard_level");
        }
        ev.entry.msg_type = safe_int(d, "msg_type", 1);

        if (ev.entry.username.empty()) return std::nullopt;
        return ev;
    } catch (const std::exception &e) {
        blog(LOG_WARNING, "[danmaku-parser] parse_open_platform_enter_room error: %s", e.what());
        return std::nullopt;
    }
}

} // namespace danmaku
