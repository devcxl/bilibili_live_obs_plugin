#include "tts-cleaner.h"
#include <QRegularExpression>

QString TtsCleaner::strip_urls(const QString &input)
{
    // 匹配 http://, https://, 或常见的域名短链
    static const QRegularExpression url_regex(
        R"((https?:\/\/[^\s]+)|(www\.[^\s]+)|([a-zA-Z0-9\-\.]+\.(com|cn|net|org|tv|cc|io|top|xyz)[^\s]*))",
        QRegularExpression::CaseInsensitiveOption
    );
    QString result = input;
    result.replace(url_regex, "");
    return result.trimmed();
}

QString TtsCleaner::compress_repeating_chars(const QString &input, int max_repeat)
{
    if (input.isEmpty() || max_repeat <= 0) return input;

    QString result;
    result.reserve(input.size());

    QChar last_char;
    int count = 0;

    for (const QChar &ch : input) {
        if (ch == last_char) {
            count++;
            if (count <= max_repeat) {
                result.append(ch);
            }
        } else {
            last_char = ch;
            count = 1;
            result.append(ch);
        }
    }
    return result;
}

QString TtsCleaner::clean_emojis_and_symbols(const QString &input)
{
    QString result = input;

    // 1. 去除 B站 官方表情占位符方括号，如 [doge] -> doge 或直接去掉常见无朗读意义表情
    static const QRegularExpression bili_emoji(R"(\[[^\]]{1,10}\])");
    result.replace(bili_emoji, "");

    // 2. 去除连续的无意义特殊标点符号（如 ~~~~~~, ......., @@@@@）
    static const QRegularExpression excessive_symbols(R"([`~!@#$%^&*()_+=\-\[\]{}|\\:;"'<>,.?/·~！@#￥%……&*（）——+={}|【】、：；“”‘’《》，。？]{4,})");
    result.replace(excessive_symbols, "，");

    return result.trimmed();
}

QString TtsCleaner::clean_danmaku(const QString &sender, const QString &message)
{
    QString text = strip_urls(message);
    text = clean_emojis_and_symbols(text);
    text = compress_repeating_chars(text, 3);

    if (text.isEmpty()) return "";

    // 截断过长弹幕（最大 50 字）
    if (text.length() > 50) {
        text = text.left(50) + "。";
    }

    return text;
}

QString TtsCleaner::format_gift(const QString &sender, const QString &action,
                               const QString &gift_name, int num, int combo_num)
{
    QString s = sender.trimmed();
    QString g = gift_name.trimmed();
    QString a = action.trimmed().isEmpty() ? "送出" : action.trimmed();

    if (s.isEmpty() || g.isEmpty()) return "";

    QString result = QString("感谢%1%2%3").arg(s, a, g);
    if (num > 1) {
        result += QString("%1个").arg(num);
    }
    if (combo_num > 1) {
        result += QString("连击%1").arg(combo_num);
    }
    return result;
}

QString TtsCleaner::format_super_chat(const QString &sender, int price, const QString &message)
{
    QString s = sender.trimmed();
    QString m = strip_urls(message);
    m = clean_emojis_and_symbols(m);
    m = compress_repeating_chars(m, 3);

    if (s.isEmpty()) s = "粉丝";
    if (m.length() > 80) {
        m = m.left(80) + "。";
    }

    return QString("醒目留言，%1说：%2").arg(s, m);
}

QString TtsCleaner::merge_messages(const std::vector<TtsMessage> &messages, size_t max_chars)
{
    if (messages.empty()) return "";
    if (messages.size() == 1) return messages[0].text;

    QString merged;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto &msg = messages[i];
        if (msg.text.isEmpty()) continue;

        if (!merged.isEmpty()) {
            // 用中文分号或逗号连接
            merged += "；";
        }
        merged += msg.text;

        if (static_cast<size_t>(merged.length()) >= max_chars) {
            break;
        }
    }
    return merged;
}
