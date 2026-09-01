#pragma once

#include <QObject>
#include <QByteArray>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QBuffer>
#include <memory>

class PcmAudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit PcmAudioPlayer(int sample_rate = 24000, QObject *parent = nullptr);
    ~PcmAudioPlayer() override;

    void set_sample_rate(int sample_rate);
    int sample_rate() const { return sample_rate_; }

    void set_volume(float volume); // 0.0 ~ 1.0
    float volume() const;

    // 整段 PCM 播放
    void play(const QByteArray &pcm_data);

    // 停止播放
    void stop();

    bool is_playing() const;

    // 工具方法：生成用于离线测试的 Mock 正弦波 PCM 音频 (440Hz A音)
    static QByteArray generate_mock_sine_pcm(int sample_rate = 24000,
                                             double duration_sec = 2.0,
                                             double frequency = 440.0);

    // 工具方法：将 Raw PCM 保存为标准 WAV 文件
    static bool save_as_wav(const QString &file_path, const QByteArray &pcm_data,
                            int sample_rate = 24000, int channels = 1, int bits_per_sample = 16);

signals:
    void playback_started();
    void playback_finished();
    void error_occurred(const QString &err);

private slots:
    void on_state_changed(QAudio::State state);

private:
    void setup_audio_sink();

    int sample_rate_ = 24000;
    float volume_ = 1.0f;
    std::unique_ptr<QAudioSink> audio_sink_;
    std::unique_ptr<QBuffer> buffer_;
    QByteArray pcm_data_;
};
