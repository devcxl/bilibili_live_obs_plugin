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
    // 解析官方开放平台业务命令 JSON
    static std::optional<ParsedEvent> parse(const std::string &json_str);

private:
    // 官方开放平台命令解析
    static std::optional<ParsedEvent> parse_open_platform_dm(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_gift(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_super_chat(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_guard(const nlohmann::json &j);
    static std::optional<ParsedEvent> parse_open_platform_enter_room(const nlohmann::json &j);
};

} // namespace danmaku
