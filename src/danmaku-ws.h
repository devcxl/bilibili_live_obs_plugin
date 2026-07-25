#pragma once

#include <string>
#include <QMetaType>

// ─── 弹幕消息 ───
struct DanmakuMessage {
    std::string cmd;           // "DANMU_MSG"
    std::string username;      // 发送者昵称
    std::string uid;           // 发送者 UID
    std::string message;       // 弹幕文本
    std::string fan_badge;     // 粉丝勋章名（可选）
    int fan_badge_level = 0;   // 粉丝勋章等级
};

// ─── 礼物消息 ───
struct GiftMessage {
    std::string username;      // 送礼者昵称
    std::string gift_name;     // 礼物名称
    int num = 0;               // 数量
    int combo_num = 0;         // 连送次数（combo）
    std::string action;        // "赠送" / "续费"
};

// ─── SC 醒目留言 ───
struct SuperChatMessage {
    std::string username;      // 发送者昵称
    std::string message;       // SC 内容
    int price = 0;             // 金额（人民币，单位：分）
};

// 注册到 Qt 元对象系统（支持跨线程信号/槽值类型传递）
Q_DECLARE_METATYPE(DanmakuMessage)
Q_DECLARE_METATYPE(GiftMessage)
Q_DECLARE_METATYPE(SuperChatMessage)
