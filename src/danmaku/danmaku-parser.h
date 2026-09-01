#pragma once

#include <string>
#include <optional>
#include "../danmaku-ws.h"
#include "danmaku-packet.h"

namespace danmaku {

enum class EventType {
    None = 0,
    Danmaku,
    Gift,
    SuperChat,
    Entry
};

struct ParsedEvent {
    EventType type = EventType::None;
    DanmakuMessage danmaku;
    GiftMessage gift;
    SuperChatMessage super_chat;
    EntryMessage entry;
};

class DanmakuParser {
public:
    // 解析 JSON 字符串，优先匹配 LIVE_OPEN_PLATFORM_* 官方命令，同时向下兼容 Web 消息
    static std::optional<ParsedEvent> parse(const std::string &json_str);

private:
    // 官方开放平台命令解析器
    static std::optional<ParsedEvent> parse_open_platform_dm(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_gift(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_super_chat(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_guard(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_enter_room(const nlohmann::json &j);

    // Web 客户端私有命令解析器
    static std::optional<ParsedEvent> parse_web_dm(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_web_gift(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_web_super_chat(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_web_interact_word(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_web_entry_effect(const nlohmann::json &j);
};

} // namespace danmaku
