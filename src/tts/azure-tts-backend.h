#pragma once

#include "tts-backend.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>

class AzureTtsBackend : public ITtsBackend {
    Q_OBJECT

public:
    explicit AzureTtsBackend(const TtsConfig &config, QObject *parent = nullptr);
    ~AzureTtsBackend() override;

    void update_config(const TtsConfig &config) override;
    const TtsConfig &config() const override { return config_; }

    void synthesize(const QString &text) override;
    void cancel() override;

    static QString build_ssml(const QString &text, const QString &voice,
                              const QString &rate = "+0%", const QString &pitch = "+0%");
    static QString escape_xml(const QString &input);

private slots:
    void on_ready_read();
    void on_reply_finished();
    void on_reply_error(QNetworkReply::NetworkError code);

private:
    TtsConfig config_;
    QNetworkAccessManager net_mgr_;
    QNetworkReply *current_reply_ = nullptr;

    QElapsedTimer timer_;
    bool first_chunk_received_ = false;
    qint64 ttfb_ms_ = 0;
    QByteArray pcm_buffer_;
};
