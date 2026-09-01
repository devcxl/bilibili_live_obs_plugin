#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <chrono>

struct AzureTtsConfig {
    QString key;
    QString region = "eastasia";
    QString voice = "zh-CN-XiaoxiaoNeural";
    QString rate = "+0%";
    QString pitch = "+0%";
    QString format = "raw-24khz-16bit-mono-pcm";

    int sample_rate() const {
        if (format.contains("16khz")) return 16000;
        if (format.contains("24khz")) return 24000;
        if (format.contains("48khz")) return 48000;
        return 24000;
    }
};

struct TtsMetrics {
    qint64 ttfb_ms = 0;              // Time To First Byte
    qint64 total_request_ms = 0;     // 总网络耗时
    qint64 pcm_bytes = 0;            // 接收的 PCM 字节数
    double audio_duration_sec = 0.0; // 音频理论时长（秒）
    double rtf = 0.0;                // Real-Time Factor (网络耗时 / 音频时长)
    int http_status = 0;
    QString error_string;
};

class AzureTtsClient : public QObject {
    Q_OBJECT

public:
    explicit AzureTtsClient(const AzureTtsConfig &config, QObject *parent = nullptr);
    ~AzureTtsClient() override = default;

    void update_config(const AzureTtsConfig &config);
    const AzureTtsConfig &config() const { return config_; }

    // 发起异步合成请求
    void synthesize(const QString &text);

    // 取消当前请求
    void cancel();

    // 静态辅助：构建 SSML 与 XML 转义
    static QString build_ssml(const QString &text, const QString &voice,
                              const QString &rate = "+0%", const QString &pitch = "+0%");
    static QString escape_xml(const QString &input);

signals:
    // 流式分片到达（可用于边下载边送入播放缓冲）
    void chunk_received(const QByteArray &chunk);
    // 合成完成
    void finished(bool success, const QByteArray &pcm_data, const TtsMetrics &metrics);
    // 错误通知
    void error_occurred(const QString &error_msg, int http_status);

private slots:
    void on_ready_read();
    void on_reply_finished();
    void on_reply_error(QNetworkReply::NetworkError code);

private:
    AzureTtsConfig config_;
    QNetworkAccessManager net_mgr_;
    QNetworkReply *current_reply_ = nullptr;

    QElapsedTimer timer_;
    bool first_chunk_received_ = false;
    qint64 ttfb_ms_ = 0;
    QByteArray pcm_buffer_;
};
