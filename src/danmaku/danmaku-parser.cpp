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
    if (cmd == cmd::ENTER_ROOM) {
        return parse_open_platform_enter_room(j);
    }

    // ── Web 客户端私有协议向下兼容 ──
    if (cmd == "DANMU_MSG") {
        return parse_web_dm(j);
    }
    if (cmd == "SEND_GIFT") {
        return parse_web_gift(j);
    }
    if (cmd == "SUPER_CHAT_MESSAGE") {
        return parse_web_super_chat(j);
    }
    if (cmd == "INTERACT_WORD") {
        return parse_web_interact_word(j);
    }
    if (cmd == "ENTRY_EFFECT") {
        return parse_web_entry_effect(j);
    }

    return std::nullopt;
}

// ── 官方开放平台解析实现 ──

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
    // 官方 rmb 为元（如 50.0），内部统一以分记录（5000）
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
    ev.entry.username = d.value("uname", "");
    ev.entry.uid = d.value("open_id", "");
    ev.entry.msg_type = 1;

    if (ev.entry.username.empty()) return std::nullopt;
    return ev;
}

// ── Web 客户端私有协议解析实现 ──

std::optional<ParsedEvent> DanmakuParser::parse_web_dm(const nlohmann::json &j)
{
    if (!j.contains("info") || !j["info"].is_array() || j["info"].size() < 4) {
        return std::nullopt;
    }
    const auto &info = j["info"];

    ParsedEvent ev;
    ev.type = EventType::Danmaku;
    ev.danmaku.cmd = "DANMU_MSG";

    if (info.size() > 1 && info[1].is_string()) {
        ev.danmaku.message = info[1].get<std::string>();
    }
    if (info.size() > 2 && info[2].is_array() && info[2].size() >= 2) {
        if (info[2][1].is_string()) {
            ev.danmaku.username = info[2][1].get<std::string>();
        }
        if (info[2][0].is_number()) {
            ev.danmaku.uid = std::to_string(info[2][0].get<int64_t>());
        }
    }
    if (info.size() > 3 && info[3].is_array() && info[3].size() >= 2) {
        if (info[3][1].is_string()) {
            ev.danmaku.fan_badge = info[3][1].get<std::string>();
        }
        if (info[3][0].is_number()) {
            ev.danmaku.fan_badge_level = info[3][0].get<int>();
        }
        if (info[3].size() > 10 && info[3][10].is_number()) {
            ev.danmaku.guard_level = info[3][10].get<int>();
        }
    }

    if (ev.danmaku.username.empty() || ev.danmaku.message.empty()) {
        return std::nullopt;
    }
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_web_gift(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Gift;
    ev.gift.username = d.value("uname", "");
    ev.gift.gift_name = d.value("giftName", "");
    ev.gift.num = d.value("num", 1);
    ev.gift.combo_num = d.value("combo_num", 0);
    ev.gift.action = d.value("action", "赠送");

    if (ev.gift.username.empty() || ev.gift.gift_name.empty()) {
        return std::nullopt;
    }
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_web_super_chat(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::SuperChat;
    ev.super_chat.username = d.value("uname", "");
    ev.super_chat.message = d.value("message", "");
    ev.super_chat.price = d.value("price", 0);

    if (ev.super_chat.username.empty() || ev.super_chat.message.empty()) {
        return std::nullopt;
    }
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_web_interact_word(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Entry;
    ev.entry.username = d.value("uname", "");
    if (d.contains("uid")) {
        if (d["uid"].is_number()) {
            ev.entry.uid = std::to_string(d["uid"].get<int64_t>());
        } else if (d["uid"].is_string()) {
            ev.entry.uid = d["uid"].get<std::string>();
        }
    }
    ev.entry.msg_type = d.value("msg_type", 1);
    if (d.contains("fans_medal") && d["fans_medal"].is_object()) {
        const auto &fm = d["fans_medal"];
        ev.entry.medal_name = fm.value("medal_name", "");
        ev.entry.medal_level = fm.value("medal_level", 0);
        ev.entry.guard_level = fm.value("guard_level", 0);
    }

    if (ev.entry.username.empty()) return std::nullopt;
    return ev;
}

std::optional<ParsedEvent> DanmakuParser::parse_web_entry_effect(const nlohmann::json &j)
{
    if (!j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const auto &d = j["data"];

    ParsedEvent ev;
    ev.type = EventType::Entry;

    std::string cw = d.value("copy_writing", "");
    if (cw.empty()) {
        cw = d.value("copy_writing_v2", "");
    }
    auto start = cw.find("<%");
    auto end = cw.find("%>");
    if (start != std::string::npos && end != std::string::npos && end > start + 2) {
        ev.entry.username = cw.substr(start + 2, end - start - 2);
    } else {
        ev.entry.username = d.value("uname", "");
    }

    if (d.contains("uid")) {
        if (d["uid"].is_number()) {
            ev.entry.uid = std::to_string(d["uid"].get<int64_t>());
        } else if (d["uid"].is_string()) {
            ev.entry.uid = d["uid"].get<std::string>();
        }
    }
    ev.entry.guard_level = d.value("privilege_type", 3);
    ev.entry.msg_type = 1;

    if (ev.entry.username.empty()) return std::nullopt;
    return ev;
}

} // namespace danmaku
