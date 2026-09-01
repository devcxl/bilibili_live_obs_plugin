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
    if (cmd == cmd::ENTER_ROOM || cmd == "INTERACT_WORD") {
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
    ev.gift.gift_name = d.value("gift_name", "");
    ev.gift.num = d.value("gift_num", 1);
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
    ev.super_chat.username = d.value("uname", "");
    ev.super_chat.message = d.value("message", "");
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
    ev.type = EventType::Gift;

    std::string uname = d.value("uname", "");
    if (uname.empty() && d.contains("user_info") && d["user_info"].is_object()) {
        uname = d["user_info"].value("uname", "");
    }
    ev.gift.username = uname;

    int guard_level = d.value("guard_level", 3);
    std::string guard_name = "舰长";
    if (guard_level == 1) guard_name = "总督";
    else if (guard_level == 2) guard_name = "提督";

    ev.gift.gift_name = guard_name;
    ev.gift.num = d.value("guard_num", 1);
    ev.gift.action = "开通";

    if (ev.gift.username.empty()) return std::nullopt;
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_open_platform_enter_room(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Entry;
    ev.entry.username = d.value("uname", d.value("nickname", ""));
    ev.entry.uid = d.value("open_id", "");
    ev.entry.guard_level = d.value("guard_level", 0);
    ev.entry.medal_name = d.value("fans_medal_name", "");
    ev.entry.medal_level = d.value("fans_medal_level", 0);
    ev.entry.msg_type = d.value("msg_type", 1);

    if (ev.entry.username.empty()) return std::nullopt;
    return ev;
}

} // namespace danmaku
