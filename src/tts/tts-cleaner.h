#pragma once

#include <QString>
#include <vector>
#include "tts-types.h"

class TtsCleaner {
public:
    // 清洗普通弹幕内容
    static QString clean_danmaku(const QString &sender, const QString &message);

    // 格式化礼物播报文本
    static QString format_gift(const QString &sender, const QString &action,
                               const QString &gift_name, int num, int combo_num);

    // 格式化 SC 醒目留言文本
    static QString format_super_chat(const QString &sender, int price, const QString &message);

    // 格式化大航海开通/上舰播报文本
    static QString format_guard(const QString &sender, int guard_level, int num, const QString &unit);

    // 格式化点赞播报文本
    static QString format_like(const QString &sender, int64_t count);

    // 格式化进房欢迎文本
    static QString format_entry(const QString &sender, int guard_level, const QString &medal_name);

    // 智能批量合并多条排队的普通弹幕或进房消息
    static QString merge_messages(const std::vector<TtsMessage> &messages, size_t max_chars = 120);

    // 去除重复刷屏字符（如 66666666 -> 666）
    static QString compress_repeating_chars(const QString &input, int max_repeat = 3);

    // 过滤 URL 链接
    static QString strip_urls(const QString &input);

    // 过滤不可读表情符号
    static QString clean_emojis_and_symbols(const QString &input);
};
