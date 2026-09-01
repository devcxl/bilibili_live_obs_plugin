#include "danmaku-parser.h"
#include <obs-module.h>

namespace danmaku {

std::optional<ParsedEvent> DanmakuParser::parse(const std::string &json_str)
{
    auto j = nlohmann::json::parse(json_str, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    std::string cmd = j.value("cmd", "");
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

    return std::nullopt;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_dm(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Danmaku;
    ev.danmaku.cmd = cmd::DM;
    ev.danmaku.username = d.value("uname", "");
    ev.danmaku.message = d.value("msg", "");
    ev.danmaku.uid = d.value("open_id", "");
    ev.danmaku.fan_badge = d.value("fans_medal_name", "");
    ev.danmaku.fan_badge_level = d.value("fans_medal_level", 0);
    ev.danmaku.guard_level = d.value("guard_level", 0);
    ev.danmaku.is_admin = (d.value("is_admin", 0) == 1);
    ev.danmaku.dm_type = d.value("dm_type", 0);
    ev.danmaku.emoji_url = d.value("emoji_img_url", "");

    if (ev.danmaku.username.empty() || ev.danmaku.message.empty()) {
        return std::nullopt;
    }
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_gift(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Gift;
    ev.gift.username = d.value("uname", "");
    ev.gift.uid = d.value("open_id", "");
    ev.gift.gift_name = d.value("gift_name", "");
    ev.gift.num = d.value("gift_num", 1);
    ev.gift.price = d.value("price", 0LL);
    ev.gift.paid = d.value("paid", true);
    ev.gift.guard_level = d.value("guard_level", 0);
    ev.gift.medal_name = d.value("fans_medal_name", "");
    ev.gift.medal_level = d.value("fans_medal_level", 0);
    ev.gift.action = "赠送";

    if (d.contains("combo_info") && d["combo_info"].is_object()) {
        ev.gift.combo_num = d["combo_info"].value("combo_num", 0);
    } else {
        ev.gift.combo_num = d.value("combo_send", 0);
    }

    if (ev.gift.username.empty() || ev.gift.gift_name.empty()) {
        return std::nullopt;
    }
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_super_chat(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::SuperChat;
    ev.super_chat.message_id = d.value("message_id", 0LL);
    ev.super_chat.username = d.value("uname", "");
    ev.super_chat.uid = d.value("open_id", "");
    ev.super_chat.message = d.value("message", "");
    ev.super_chat.guard_level = d.value("guard_level", 0);
    ev.super_chat.medal_name = d.value("fans_medal_name", "");
    ev.super_chat.medal_level = d.value("fans_medal_level", 0);

    double rmb = d.value("rmb", 0.0);
    ev.super_chat.price = static_cast<int>(rmb * 100);

    if (ev.super_chat.username.empty() || ev.super_chat.message.empty()) {
        return std::nullopt;
    }
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_guard(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Guard;

    std::string uname = d.value("uname", "");
    if (uname.empty() && d.contains("user_info") && d["user_info"].is_object()) {
        uname = d["user_info"].value("uname", "");
        ev.guard.uid = d["user_info"].value("open_id", "");
    } else {
        ev.guard.uid = d.value("open_id", "");
    }
    ev.guard.username = uname;
    ev.guard.guard_level = d.value("guard_level", 3);
    ev.guard.guard_num = d.value("guard_num", 1);
    ev.guard.guard_unit = d.value("guard_unit", "月");
    ev.guard.medal_name = d.value("fans_medal_name", "");
    ev.guard.medal_level = d.value("fans_medal_level", 0);
    ev.guard.timestamp = d.value("timestamp", 0LL);

    if (ev.guard.username.empty()) return std::nullopt;
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_like(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Like;
    ev.like.username = d.value("uname", "");
    ev.like.uid = d.value("open_id", "");
    ev.like.like_text = d.value("like_text", "点赞了");
    ev.like.like_count = d.value("like_count", 1LL);
    ev.like.medal_name = d.value("fans_medal_name", "");
    ev.like.medal_level = d.value("fans_medal_level", 0);
    ev.like.timestamp = d.value("timestamp", 0LL);

    if (ev.like.username.empty()) return std::nullopt;
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_enter_room(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Entry;

    std::string uname = d.value("uname", d.value("nickname", d.value("username", "")));
    if (uname.empty() && d.contains("user_info") && d["user_info"].is_object()) {
        uname = d["user_info"].value("uname", "");
    }
    if (uname.empty() && d.contains("copy_writing")) {
        std::string cw = d.value("copy_writing", "");
        auto start = cw.find("<%");
        auto end = cw.find("%>");
        if (start != std::string::npos && end != std::string::npos && end > start + 2) {
            uname = cw.substr(start + 2, end - start - 2);
        }
    }

    ev.entry.username = uname;
    ev.entry.uid = d.value("open_id", d.value("uid", ""));
    ev.entry.guard_level = d.value("guard_level", d.value("privilege_type", 0));
    ev.entry.medal_name = d.value("fans_medal_name", "");
    ev.entry.medal_level = d.value("fans_medal_level", 0);

    if (d.contains("fans_medal") && d["fans_medal"].is_object()) {
        const auto &fm = d["fans_medal"];
        if (ev.entry.medal_name.empty()) ev.entry.medal_name = fm.value("medal_name", "");
        if (ev.entry.medal_level == 0) ev.entry.medal_level = fm.value("medal_level", 0);
        if (ev.entry.guard_level == 0) ev.entry.guard_level = fm.value("guard_level", 0);
    }
    ev.entry.msg_type = d.value("msg_type", 1);

    if (ev.entry.username.empty()) return std::nullopt;
    return ev;
}

} // namespace danmaku
