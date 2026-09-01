#pragma once

#include <QObject>
#include "tts-types.h"

class ITtsBackend : public QObject {
    Q_OBJECT

public:
    explicit ITtsBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~ITtsBackend() override = default;

    virtual void update_config(const TtsConfig &config) = 0;
    virtual const TtsConfig &config() const = 0;

    // 发起合成请求
    virtual void synthesize(const QString &text) = 0;

    // 取消当前合成
    virtual void cancel() = 0;

signals:
    void chunk_received(const QByteArray &chunk);
    void finished(bool success, const QByteArray &pcm_data, const TtsMetrics &metrics);
    void error_occurred(const QString &err_msg, int http_status);
};
