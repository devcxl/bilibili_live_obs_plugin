#include "tts-manager.h"
#include "azure-tts-backend.h"
#include "tts-cleaner.h"
#include <obs-module.h>
#include <QDateTime>
#include <algorithm>

TtsManager::TtsManager(QObject *parent)
    : QObject(parent)
{
    backend_ = std::make_unique<AzureTtsBackend>(config_, this);
    audio_sink_ = std::make_unique<PcmAudioSink>(config_.sample_rate(), this);

    connect(backend_.get(), &ITtsBackend::finished, this, &TtsManager::on_backend_finished);
    connect(backend_.get(), &ITtsBackend::error_occurred, this, [this](const QString &err, int status) {
        emit error_occurred(QString("%1 (HTTP %2)").arg(err).arg(status));
    });

    connect(audio_sink_.get(), &PcmAudioSink::playback_started, this, [this]() {
        emit tts_state_changed(true);
    });
    connect(audio_sink_.get(), &PcmAudioSink::playback_finished, this, &TtsManager::on_playback_finished);
}

TtsManager::~TtsManager()
{
    stop_and_clear();
}

void TtsManager::update_config(const TtsConfig &config)
{
    config_ = config;
    if (backend_) {
        backend_->update_config(config_);
    }
    if (audio_sink_) {
        audio_sink_->set_sample_rate(config_.sample_rate());
        audio_sink_->set_volume(config_.volume / 100.0f);
    }
    if (!config_.enabled) {
        stop_and_clear();
    }
}

void TtsManager::set_enabled(bool enabled)
{
    config_.enabled = enabled;
    if (!enabled) {
        stop_and_clear();
    }
}

void TtsManager::stop_and_clear()
{
    if (backend_) {
        backend_->cancel();
    }
    if (audio_sink_) {
        audio_sink_->stop();
    }
    in_flight_ = false;
    queue_.clear();
    emit queue_size_changed(0);
    emit tts_state_changed(false);
}

void TtsManager::test_speak(const QString &text)
{
    if (text.trimmed().isEmpty()) return;

    TtsMessage msg;
    msg.id = next_id_++;
    msg.priority = TtsPriority::SuperChat; // 试听使用最高优先级
    msg.text = text.trimmed();
    msg.sender = "系统测试";
    msg.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(msg);
}

void TtsManager::on_danmaku_received(const DanmakuMessage &msg)
{
    if (!config_.enabled || !config_.read_danmaku) return;

    QString cleaned = TtsCleaner::clean_danmaku(
        QString::fromStdString(msg.username),
        QString::fromStdString(msg.message)
    );
    if (cleaned.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Normal;
    item.text = cleaned;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_gift_received(const GiftMessage &msg)
{
    if (!config_.enabled || !config_.read_gift) return;

    QString formatted = TtsCleaner::format_gift(
        QString::fromStdString(msg.username),
        QString::fromStdString(msg.action),
        QString::fromStdString(msg.gift_name),
        msg.num,
        msg.combo_num
    );
    if (formatted.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Gift;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_super_chat_received(const SuperChatMessage &msg)
{
    if (!config_.enabled || !config_.read_sc) return;

    QString formatted = TtsCleaner::format_super_chat(
        QString::fromStdString(msg.username),
        msg.price,
        QString::fromStdString(msg.message)
    );
    if (formatted.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::SuperChat;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_entry_received(const EntryMessage &msg)
{
    if (!config_.enabled || !config_.read_entry) return;

    // 过滤范围判断
    if (config_.entry_filter == TtsEntryFilter::GuardOnly) {
        if (msg.guard_level <= 0) return; // 非大航海忽略
    } else if (config_.entry_filter == TtsEntryFilter::WithMedal) {
        if (msg.medal_name.empty() && msg.guard_level <= 0) return; // 无勋章且非大航海忽略
    }

    QString formatted = TtsCleaner::format_entry(
        QString::fromStdString(msg.username),
        msg.guard_level,
        QString::fromStdString(msg.medal_name)
    );
    if (formatted.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Entry;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::enqueue_message(const TtsMessage &msg)
{
    if (queue_.size() >= MAX_QUEUE_SIZE) {
        // 队列达到 1000 上限时：优先淘汰队尾最老的 Entry，其次 Normal，确保 SC 和礼物不丢失
        bool removed = false;
        for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
            if (it->priority == TtsPriority::Entry) {
                queue_.erase(std::next(it).base());
                removed = true;
                break;
            }
        }
        if (!removed) {
            for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
                if (it->priority == TtsPriority::Normal) {
                    queue_.erase(std::next(it).base());
                    removed = true;
                    break;
                }
            }
        }
        if (!removed) {
            queue_.pop_front();
        }
    }

    // 按优先级插入：SC (3) > Gift (2) > Normal (1) > Entry (0)
    if (msg.priority == TtsPriority::SuperChat) {
        auto it = std::find_if(queue_.begin(), queue_.end(), [](const TtsMessage &m) {
            return m.priority < TtsPriority::SuperChat;
        });
        queue_.insert(it, msg);
    } else if (msg.priority == TtsPriority::Gift) {
        auto it = std::find_if(queue_.begin(), queue_.end(), [](const TtsMessage &m) {
            return m.priority < TtsPriority::Gift;
        });
        queue_.insert(it, msg);
    } else if (msg.priority == TtsPriority::Normal) {
        auto it = std::find_if(queue_.begin(), queue_.end(), [](const TtsMessage &m) {
            return m.priority < TtsPriority::Normal;
        });
        queue_.insert(it, msg);
    } else {
        queue_.push_back(msg);
    }

    emit queue_size_changed(queue_.size());
    process_queue();
}

void TtsManager::process_queue()
{
    if (!config_.enabled) return;
    if (in_flight_ || audio_sink_->is_playing()) return;
    if (queue_.empty()) {
        emit tts_state_changed(false);
        return;
    }

    // 取出待合成内容
    TtsMessage first = queue_.front();
    queue_.pop_front();

    QString text_to_speak;

    if (first.priority == TtsPriority::Normal && config_.merge_enabled && !queue_.empty()) {
        // 尝试批量合并紧接着的连续普通弹幕（最多 4 条，总长度 ≤ 100 字）
        std::vector<TtsMessage> batch;
        batch.push_back(first);

        while (!queue_.empty() && queue_.front().priority == TtsPriority::Normal && batch.size() < 4) {
            batch.push_back(queue_.front());
            queue_.pop_front();
        }
        text_to_speak = TtsCleaner::merge_messages(batch, 100);
    } else if (first.priority == TtsPriority::Entry && config_.merge_enabled && !queue_.empty()) {
        // 尝试批量合并紧接着的连续进房消息（最多 3 条）
        std::vector<TtsMessage> batch;
        batch.push_back(first);

        while (!queue_.empty() && queue_.front().priority == TtsPriority::Entry && batch.size() < 3) {
            batch.push_back(queue_.front());
            queue_.pop_front();
        }
        text_to_speak = TtsCleaner::merge_messages(batch, 80);
    } else {
        text_to_speak = first.text;
    }

    emit queue_size_changed(queue_.size());

    if (text_to_speak.trimmed().isEmpty()) {
        // 若为空，继续下一轮
        process_queue();
        return;
    }

    in_flight_ = true;
    backend_->synthesize(text_to_speak);
}

void TtsManager::on_backend_finished(bool success, const QByteArray &pcm_data, const TtsMetrics &metrics)
{
    in_flight_ = false;

    if (success && !pcm_data.isEmpty()) {
        audio_sink_->play(pcm_data);
    } else {
        blog(LOG_WARNING, "[TTS] 合成未产生有效音频，跳过并继续下一项: %s",
             metrics.error_string.toUtf8().constData());
        // 失败时继续调度下一项，防止队列停滞
        process_queue();
    }
}

void TtsManager::on_playback_finished()
{
    // 当前项播放完毕，继续处理队列中下一项
    process_queue();
}
