# Azure TTS + QAudioSink PoC 验证工具

用于验证 B站直播弹幕插件中基于 **Azure Speech REST API + Qt 6 QAudioSink** 的轻量级语音合成与播放方案。

## 特性

- **零外部 SDK 依赖**：纯 Qt 6（`Qt6::Network` + `Qt6::Multimedia`），无需引入微软 SDK 或庞大模型。
- **Raw PCM 流式接收**：直接请求 `raw-24khz-16bit-mono-pcm`，避免 MP3 解码开销。
- **详细性能指标统计**：自动计算首包耗时（TTFB）、总耗时、音频时长、实时因子（RTF）。
- **多场景弹幕测试套件**：内置短弹幕、中英混合、数字、标点表情、多条合并与长文本 SC 测试。
- **WAV 文件导出**：可导出标准 RIFF/WAVE 文件以便离线检查音质。
- **离线 Mock 模式**：无 Key 状态下自动生成 440Hz 正弦波验证音频输出与事件循环。

## 编译方法

```bash
cmake -B build-poc -S tools/tts-poc
cmake --build build-poc
```

## 使用方法

### 1. 离线 Mock 验证（无需 Key）

```bash
./build-poc/azure_tts_poc --mock --save-wav /tmp/mock_test.wav
```

### 2. 在线单句合成测试

```bash
export AZURE_SPEECH_KEY="你的_Azure_Speech_Key"
export AZURE_SPEECH_REGION="eastasia"

./build-poc/azure_tts_poc --text "感谢关注哔哩哔哩直播间！" --save-wav /tmp/danmaku.wav
```

### 3. 多场景弹幕批量压测（Benchmark）

```bash
./build-poc/azure_tts_poc --key <your_key> --region eastasia --batch-test --no-play
```
