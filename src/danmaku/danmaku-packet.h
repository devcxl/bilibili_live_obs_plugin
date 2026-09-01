#pragma once

#include <cstdint>
#include <string>
#include <QByteArray>

namespace danmaku {

// Bilibili 官方长链数据协议包头固定长度 (16 字节)
constexpr uint16_t HEADER_LENGTH = 16;

// 协议版本 (Protocol Version)
enum class ProtoVer : uint16_t {
    Normal     = 0, // 普通 JSON 文本
    Popularity = 1, // 人气值 / 心跳 (Int32 Big Endian)
    Zlib       = 2, // Zlib / Deflate 压缩包
    Brotli     = 3  // Brotli 压缩包
};

// 操作码 (Operation Code)
enum class OpCode : uint32_t {
    Heartbeat      = 2, // 客户端发送心跳包 (30 秒一次)
    HeartbeatReply = 3, // 服务端心跳回复 (包含人气值或状态)
    Message        = 5, // 服务端业务通知推送 (弹幕/礼物/SC/进房等)
    Auth           = 7, // 客户端发送认证包 (建连后首包)
    AuthReply      = 8  // 服务端认证结果回复
};

// 官方开放平台业务命令标识常量
namespace cmd {
    inline constexpr const char *DM              = "LIVE_OPEN_PLATFORM_DM";
    inline constexpr const char *SEND_GIFT       = "LIVE_OPEN_PLATFORM_SEND_GIFT";
    inline constexpr const char *SUPER_CHAT      = "LIVE_OPEN_PLATFORM_SUPER_CHAT";
    inline constexpr const char *SUPER_CHAT_DEL  = "LIVE_OPEN_PLATFORM_SUPER_CHAT_DEL";
    inline constexpr const char *GUARD           = "LIVE_OPEN_PLATFORM_GUARD";
    inline constexpr const char *LIKE            = "LIVE_OPEN_PLATFORM_LIKE";
    inline constexpr const char *ENTER_ROOM      = "LIVE_OPEN_PLATFORM_LIVE_ROOM_ENTER";
    inline constexpr const char *LIVE_START      = "LIVE_OPEN_PLATFORM_LIVE";
    inline constexpr const char *LIVE_END        = "LIVE_OPEN_PLATFORM_LIVE_OFF";
}

// 二进制协议帧数据结构
struct RawPacket {
    uint32_t packet_length = HEADER_LENGTH;
    uint16_t header_length = HEADER_LENGTH;
    uint16_t protover = static_cast<uint16_t>(ProtoVer::Normal);
    uint32_t op = static_cast<uint32_t>(OpCode::Message);
    uint32_t seq = 1;
    QByteArray body;
};

} // namespace danmaku
