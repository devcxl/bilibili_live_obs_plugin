#pragma once

#include <QString>
#include <QByteArray>
#include <cstdint>

enum class TtsPriority {
    Normal = 0,   // 普通弹幕
    Gift = 1,     // 礼物消息
    SuperChat = 2 // SC 醒目留言
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
