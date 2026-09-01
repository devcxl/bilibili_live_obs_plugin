#pragma once

#include <QString>
#include <QByteArray>
#include <cstdint>

enum class TtsPriority {
    Like = 0,     // 点赞（最低优先级）
    Entry = 1,    // 进房欢迎
    Normal = 2,   // 普通弹幕
    Gift = 3,     // 礼物消息
    SuperChat = 4,// SC 醒目留言
    Guard = 5     // 大航海开通/上舰（最高优先级）
};

enum class TtsEntryFilter {
    All = 0,       // 全部观众进房
    WithMedal = 1, // 仅佩戴粉丝勋章的观众
    GuardOnly = 2  // 仅大航海（舰长/提督/总督）
};

struct TtsMessage {
    uint64_t id = 0;
    TtsPriority priority = TtsPriority::Normal;
    QString text;
    QString sender;
    int64_t timestamp_ms = 0;

    // 比较运算符：用于优先队列排序（SC > Gift > Normal；同优先级时间先者先播）
    bool operator<(const TtsMessage &other) const {
        if (priority != other.priority) {
            return static_cast<int>(priority) < static_cast<int>(other.priority);
        }
        return timestamp_ms > other.timestamp_ms;
    }
};

struct TtsConfig {
    bool enabled = false;
    QString key;
    QString region = "eastasia";
    QString voice = "zh-CN-XiaoxiaoNeural";
    QString rate = "+0%";
    QString pitch = "+0%";
    int volume = 100; // 0 ~ 100
    bool read_danmaku = true;
    bool read_gift = true;
    bool read_sc = true;
    bool read_guard = true;
    bool read_like = false;
    bool read_entry = true;
    TtsEntryFilter entry_filter = TtsEntryFilter::All;
    bool merge_enabled = true;

    QString format = "raw-24khz-16bit-mono-pcm";

    int sample_rate() const {
        if (format.contains("16khz")) return 16000;
        if (format.contains("24khz")) return 24000;
        if (format.contains("48khz")) return 48000;
        return 24000;
    }
};

struct TtsMetrics {
    qint64 ttfb_ms = 0;
    qint64 total_request_ms = 0;
    qint64 pcm_bytes = 0;
    double audio_duration_sec = 0.0;
    double rtf = 0.0;
    int http_status = 0;
    QString error_string;
};
