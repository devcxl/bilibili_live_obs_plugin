#include <QGuiApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QProcessEnvironment>
#include <QTimer>
#include <QDebug>
#include <iostream>
#include <iomanip>
#include <vector>

#include "azure_tts_client.h"
#include "pcm_audio_player.h"

struct TestCase {
    QString label;
    QString text;
};

int main(int argc, char *argv[])
{
    // 使用 QGuiApplication 确保音频与多媒体事件循环完整初始化
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("azure_tts_poc");
    QGuiApplication::setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Bilibili Live OBS Plugin - Azure Speech REST + QAudioSink TTS PoC");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption keyOpt({"k", "key"}, "Azure Speech Subscription Key", "key");
    QCommandLineOption regionOpt({"r", "region"}, "Azure Speech Region (默认: eastasia)", "region", "eastasia");
    QCommandLineOption voiceOpt({"V", "voice"}, "Azure Voice Name (默认: zh-CN-XiaoxiaoNeural)", "voice", "zh-CN-XiaoxiaoNeural");
    QCommandLineOption rateOpt("rate", "Prosody rate (例如: +10%, 默认: +0%)", "rate", "+0%");
    QCommandLineOption pitchOpt("pitch", "Prosody pitch (例如: +5%, 默认: +0%)", "pitch", "+0%");
    QCommandLineOption volumeOpt("volume", "Volume 0-100 (默认: 100)", "volume", "100");
    QCommandLineOption formatOpt("format", "Audio format (默认: raw-24khz-16bit-mono-pcm)", "format", "raw-24khz-16bit-mono-pcm");
    QCommandLineOption textOpt({"t", "text"}, "Text to synthesize", "text");
    QCommandLineOption batchOpt({"b", "batch-test"}, "Run built-in multi-scenario danmaku test suite");
    QCommandLineOption saveWavOpt({"s", "save-wav"}, "Save output PCM to a standard WAV file", "path");
    QCommandLineOption mockOpt({"m", "mock"}, "Run in offline mock mode (generate 440Hz sine wave PCM)");
    QCommandLineOption noPlayOpt("no-play", "Do not play audio to speakers (network/benchmark only)");

    parser.addOption(keyOpt);
    parser.addOption(regionOpt);
    parser.addOption(voiceOpt);
    parser.addOption(rateOpt);
    parser.addOption(pitchOpt);
    parser.addOption(volumeOpt);
    parser.addOption(formatOpt);
    parser.addOption(textOpt);
    parser.addOption(batchOpt);
    parser.addOption(saveWavOpt);
    parser.addOption(mockOpt);
    parser.addOption(noPlayOpt);

    parser.process(app);

    auto env = QProcessEnvironment::systemEnvironment();
    QString key = parser.value(keyOpt);
    if (key.isEmpty()) {
        key = env.value("AZURE_SPEECH_KEY");
    }
    QString region = parser.value(regionOpt);
    if (region == "eastasia" && env.contains("AZURE_SPEECH_REGION")) {
        region = env.value("AZURE_SPEECH_REGION");
    }

    AzureTtsConfig config;
    config.key = key;
    config.region = region;
    config.voice = parser.value(voiceOpt);
    if (config.voice.trimmed().isEmpty()) {
        config.voice = "zh-CN-XiaoxiaoNeural";
    }
    config.rate = parser.value(rateOpt);
    config.pitch = parser.value(pitchOpt);
    config.format = parser.value(formatOpt);

    int volume_int = parser.value(volumeOpt).toInt();
    float volume_float = std::clamp(volume_int / 100.0f, 0.0f, 1.0f);

    bool is_mock = parser.isSet(mockOpt);
    bool is_batch = parser.isSet(batchOpt);
    bool no_play = parser.isSet(noPlayOpt);
    QString save_wav_path = parser.value(saveWavOpt);
    QString custom_text = parser.value(textOpt);

    std::cout << "========================================================\n";
    std::cout << " Bilibili Live OBS - Azure TTS + QAudioSink PoC\n";
    std::cout << "========================================================\n";
    std::cout << "Region      : " << config.region.toStdString() << "\n";
    std::cout << "Voice       : " << config.voice.toStdString() << "\n";
    std::cout << "Format      : " << config.format.toStdString() << " (" << config.sample_rate() << " Hz)\n";
    std::cout << "Rate / Pitch: " << config.rate.toStdString() << " / " << config.pitch.toStdString() << "\n";
    std::cout << "Volume      : " << (volume_float * 100) << "%\n";
    std::cout << "Mode        : " << (is_mock ? "Mock Offline" : (config.key.isEmpty() ? "No Key (Auto Mock fallback)" : "Online Azure REST")) << "\n";
    std::cout << "--------------------------------------------------------\n";

    // 注意：不要传 &app 作为 parent，避免 QObject 树与 std::shared_ptr 发生 double free
    auto player = std::make_shared<PcmAudioPlayer>(config.sample_rate(), nullptr);
    player->set_volume(volume_float);

    auto client = std::make_shared<AzureTtsClient>(config, nullptr);

    // Mock 模式
    if (is_mock || (config.key.isEmpty() && !is_batch)) {
        if (!is_mock) {
            std::cout << "\n[提示] 未检测到 AZURE_SPEECH_KEY，自动进入 Mock 离线测试模式。\n";
            std::cout << "       如需在线测试，请使用 --key <key> 或 export AZURE_SPEECH_KEY=<key>\n\n";
        }
        std::cout << "[Mock] 正在生成 2.0 秒 440Hz 标准 PCM 音频...\n";
        QByteArray mock_pcm = PcmAudioPlayer::generate_mock_sine_pcm(config.sample_rate(), 2.0, 440.0);
        std::cout << "[Mock] PCM 数据大小: " << mock_pcm.size() << " 字节\n";

        if (!save_wav_path.isEmpty()) {
            if (PcmAudioPlayer::save_as_wav(save_wav_path, mock_pcm, config.sample_rate())) {
                std::cout << "[Mock] 已成功保存 WAV 文件: " << save_wav_path.toStdString() << "\n";
            } else {
                std::cerr << "[Mock] 保存 WAV 文件失败: " << save_wav_path.toStdString() << "\n";
            }
        }

        if (!no_play) {
            std::cout << "[Mock] 开始播放音频 (QAudioSink)..." << std::endl;
            QObject::connect(player.get(), &PcmAudioPlayer::playback_finished, [&app]() {
                std::cout << "[Mock] 播放完成！PoC 验证通过。" << std::endl;
                app.quit();
            });
            player->play(mock_pcm);
        } else {
            std::cout << "[Mock] (--no-play) 跳过扬声器播放。PoC 验证通过。" << std::endl;
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
        }
        return app.exec();
    }

    // 批量测试集
    std::vector<TestCase> test_cases;
    if (is_batch) {
        test_cases = {
            {"短弹幕", "主播666！"},
            {"中英数字", "关注主播直播间，今天冲到Lv10，RoomID: 123456"},
            {"标点网络梗", "？？？这也行？？卧槽绝了，蚌埠住了哈哈哈哈！"},
            {"合并批量弹幕", "感谢【半颗白菜】投喂的辣条；感谢【小明】送出的小星星；【张三】说：主播今晚几点下播呀？"},
            {"长文本SC", "这是一条价值50元的醒目留言：主播今天直播辛苦了，非常期待下一期视频的更新，加油！"}
        };
    } else {
        QString text = custom_text.isEmpty() ? "欢迎来到哔哩哔哩直播间，感谢你的关注与弹幕支持！" : custom_text;
        test_cases.push_back({"自定义测试", text});
    }

    struct ResultItem {
        QString label;
        int text_len = 0;
        qint64 ttfb_ms = 0;
        qint64 total_ms = 0;
        double audio_sec = 0.0;
        double rtf = 0.0;
        bool ok = false;
    };
    auto results = std::make_shared<std::vector<ResultItem>>();
    auto current_idx = std::make_shared<size_t>(0);

    auto run_next = std::make_shared<std::function<void()>>();
    *run_next = [client, player, test_cases, results, current_idx, run_next, save_wav_path, no_play, &app]() {
        if (*current_idx >= test_cases.size()) {
            // 打印汇总表格
            std::cout << "\n========================================================================================\n";
            std::cout << "                                  Azure TTS 性能评测汇总\n";
            std::cout << "========================================================================================\n";
            std::cout << std::left
                      << std::setw(16) << "场景"
                      << std::setw(8) << "字数"
                      << std::setw(12) << "首包(TTFB)"
                      << std::setw(12) << "总网络耗时"
                      << std::setw(12) << "音频时长"
                      << std::setw(10) << "RTF (因子)"
                      << std::setw(8) << "状态"
                      << "\n";
            std::cout << "----------------------------------------------------------------------------------------\n";
            for (const auto &r : *results) {
                std::cout << std::left
                          << std::setw(16) << r.label.toStdString()
                          << std::setw(8) << r.text_len
                          << std::setw(12) << (QString::number(r.ttfb_ms) + " ms").toStdString()
                          << std::setw(12) << (QString::number(r.total_ms) + " ms").toStdString()
                          << std::setw(12) << (QString::number(r.audio_sec, 'f', 2) + " s").toStdString()
                          << std::setw(10) << QString::number(r.rtf, 'f', 3).toStdString()
                          << std::setw(8) << (r.ok ? "PASS" : "FAIL")
                          << "\n";
            }
            std::cout << "========================================================================================\n";
            app.quit();
            return;
        }

        const auto &tc = test_cases[*current_idx];
        std::cout << QString("\n[%1/%2] 正在合成【%3】(字数: %4): \"%5\"...\n")
                         .arg(*current_idx + 1)
                         .arg(test_cases.size())
                         .arg(tc.label)
                         .arg(tc.text.size())
                         .arg(tc.text)
                         .toStdString();

        client->synthesize(tc.text);
    };

    QObject::connect(client.get(), &AzureTtsClient::finished,
                     [&app, client, player, test_cases, results, current_idx, run_next, save_wav_path, no_play](bool success, const QByteArray &pcm, const TtsMetrics &m) {
        const auto &tc = test_cases[*current_idx];
        ResultItem item;
        item.label = tc.label;
        item.text_len = tc.text.size();
        item.ttfb_ms = m.ttfb_ms;
        item.total_ms = m.total_request_ms;
        item.audio_sec = m.audio_duration_sec;
        item.rtf = m.rtf;
        item.ok = success;
        results->push_back(item);

        if (!success) {
            std::cerr << "  [FAIL] 合成失败: " << m.error_string.toStdString()
                      << " (HTTP " << m.http_status << ")\n";
            (*current_idx)++;
            (*run_next)();
            return;
        }

        std::cout << QString("  [OK] 首包(TTFB): %1 ms | 总耗时: %2 ms | 音频时长: %3 s | 数据: %4 字节 | RTF: %5\n")
                         .arg(m.ttfb_ms)
                         .arg(m.total_request_ms)
                         .arg(m.audio_duration_sec, 0, 'f', 2)
                         .arg(m.pcm_bytes)
                         .arg(m.rtf, 0, 'f', 3)
                         .toStdString();

        if (!save_wav_path.isEmpty() && *current_idx == 0) {
            PcmAudioPlayer::save_as_wav(save_wav_path, pcm, client->config().sample_rate());
            std::cout << "  [Save] 已保存 WAV: " << save_wav_path.toStdString() << "\n";
        }

        if (!no_play && test_cases.size() == 1) {
            std::cout << "  [Play] 正在播放音频到扬声器 (QAudioSink)..." << std::endl;
            QObject::connect(player.get(), &PcmAudioPlayer::playback_finished, [&app]() {
                std::cout << "  [Play] 播放完毕！" << std::endl;
                app.quit();
            });
            player->play(pcm);
        } else {
            (*current_idx)++;
            QTimer::singleShot(200, &app, [run_next]() { (*run_next)(); });
        }
    });

    QObject::connect(client.get(), &AzureTtsClient::error_occurred, [](const QString &err, int status) {
        std::cerr << "  [Error] " << err.toStdString() << " (Status: " << status << ")\n";
    });

    // 启动第一项测试
    QTimer::singleShot(0, &app, [run_next]() { (*run_next)(); });

    return app.exec();
}
