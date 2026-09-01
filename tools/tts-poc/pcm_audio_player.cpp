#include "pcm_audio_player.h"
#include <QFile>
#include <QDataStream>
#include <QDebug>
#include <cmath>

PcmAudioPlayer::PcmAudioPlayer(int sample_rate, QObject *parent)
    : QObject(parent), sample_rate_(sample_rate)
{
    setup_audio_sink();
}

PcmAudioPlayer::~PcmAudioPlayer()
{
    stop();
}

void PcmAudioPlayer::setup_audio_sink()
{
    stop();

    QAudioFormat format;
    format.setSampleRate(sample_rate_);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice default_device = QMediaDevices::defaultAudioOutput();
    if (default_device.isNull()) {
        qWarning() << "[PcmAudioPlayer] 未找到可用音频输出设备";
        return;
    }

    if (!default_device.isFormatSupported(format)) {
        qWarning() << "[PcmAudioPlayer] 默认设备不原生支持该音频格式，尝试偏好转换";
    }

    audio_sink_ = std::make_unique<QAudioSink>(default_device, format, this);
    audio_sink_->setVolume(volume_);

    connect(audio_sink_.get(), &QAudioSink::stateChanged, this, &PcmAudioPlayer::on_state_changed);
}

void PcmAudioPlayer::set_sample_rate(int sample_rate)
{
    if (sample_rate_ != sample_rate) {
        sample_rate_ = sample_rate;
        setup_audio_sink();
    }
}

void PcmAudioPlayer::set_volume(float volume)
{
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (audio_sink_) {
        audio_sink_->setVolume(volume_);
    }
}

float PcmAudioPlayer::volume() const
{
    return volume_;
}

void PcmAudioPlayer::play(const QByteArray &pcm_data)
{
    stop();

    if (pcm_data.isEmpty()) {
        emit playback_finished();
        return;
    }

    if (!audio_sink_) {
        setup_audio_sink();
        if (!audio_sink_) {
            emit error_occurred("音频输出初始化失败");
            return;
        }
    }

    pcm_data_ = pcm_data;
    buffer_ = std::make_unique<QBuffer>(&pcm_data_);
    if (!buffer_->open(QIODevice::ReadOnly)) {
        emit error_occurred("无法打开内存音频缓冲区");
        return;
    }

    emit playback_started();
    audio_sink_->start(buffer_.get());
}

void PcmAudioPlayer::stop()
{
    if (audio_sink_) {
        audio_sink_->stop();
    }
    if (buffer_) {
        buffer_->close();
        buffer_.reset();
    }
    pcm_data_.clear();
}

bool PcmAudioPlayer::is_playing() const
{
    if (!audio_sink_) return false;
    return audio_sink_->state() == QAudio::ActiveState;
}

void PcmAudioPlayer::on_state_changed(QAudio::State state)
{
    if (state == QAudio::IdleState) {
        // 音频数据已全部写入声卡驱动缓冲区并播放完毕
        stop();
        emit playback_finished();
    } else if (state == QAudio::StoppedState) {
        if (audio_sink_ && audio_sink_->error() != QAudio::NoError) {
            emit error_occurred(QString("音频播放错误，代码: %1").arg(audio_sink_->error()));
        }
    }
}

QByteArray PcmAudioPlayer::generate_mock_sine_pcm(int sample_rate, double duration_sec, double frequency)
{
    int total_samples = static_cast<int>(sample_rate * duration_sec);
    QByteArray pcm;
    pcm.resize(total_samples * static_cast<int>(sizeof(int16_t)));

    auto *data = reinterpret_cast<int16_t*>(pcm.data());
    constexpr double two_pi = 2.0 * 3.14159265358979323846;
    double step = two_pi * frequency / sample_rate;

    for (int i = 0; i < total_samples; ++i) {
        double val = std::sin(step * i) * 16384.0; // 50% 音量正弦波
        data[i] = static_cast<int16_t>(val);
    }

    return pcm;
}

bool PcmAudioPlayer::save_as_wav(const QString &file_path, const QByteArray &pcm_data,
                                int sample_rate, int channels, int bits_per_sample)
{
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);

    uint32_t data_size = pcm_data.size();
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);
    uint32_t riff_chunk_size = 36 + data_size;

    // RIFF header
    file.write("RIFF", 4);
    out << riff_chunk_size;
    file.write("WAVE", 4);

    // fmt subchunk
    file.write("fmt ", 4);
    uint32_t subchunk1_size = 16;
    uint16_t audio_format = 1; // PCM
    out << subchunk1_size;
    out << audio_format;
    out << static_cast<uint16_t>(channels);
    out << static_cast<uint32_t>(sample_rate);
    out << byte_rate;
    out << block_align;
    out << static_cast<uint16_t>(bits_per_sample);

    // data subchunk
    file.write("data", 4);
    out << data_size;
    file.write(pcm_data.constData(), pcm_data.size());

    file.close();
    return true;
}
