# ADR-003: 弹幕语音播报 (TTS) 架构与实施设计

## 状态

已采纳 (Accepted)

## 上下文

为了增强主播与观众的互动体验，项目需要新增 B站直播弹幕/礼物/SC 的实时语音朗读（Text-To-Speech, TTS）功能。
在前期调研报告（`docs/dev/research/bilibili-live-danmaku-tts.md`）和原型验证（`tools/tts-poc`）中，我们确立了以下核心诉求与约束：

1. **听觉体验**：主播本机能直接听到播报，直播间观众通过 OBS 桌面音频捕获（Desktop Audio）同步听到。
2. **轻量与可维护性**：优先采用零外部 C++ SDK / 零庞大模型依赖的轻量方案，不增加 DEB/AUR 分发包体积与许可证合规风险。
3. **低延迟与性能**：网络热态首包延迟（TTFB）控制在 500ms 以内，实时因子 RTF ≤ 0.2，高频弹幕下不过度占用 CPU。
4. **强故障隔离**：TTS 模块必须作为独立的纯消费者（Sink），网络断网、鉴权失败、429 限流或音频播放异常绝对不能阻塞/影响弹幕展示、WebSocket 接收和 OBS 推流。

---

## 架构决策

### 决定 1：采用 Azure Speech 官方 REST API + Raw PCM 作为远程 TTS 后端

* **选择**：利用 Qt6 原生 `QNetworkAccessManager` 调用 Azure Cognitive Services TTS REST API，直接请求 `raw-24khz-16bit-mono-pcm`（或 16kHz）未经压缩的原始音频流。
* **排除方案**：
  * **微软 C++ Speech SDK**：引入约 30MB 二进制动态库与复杂的再分发许可条款，而 REST API 已满足低延迟需求。
  * **本地神经网络（sherpa-onnx / piper）**：引入 100MB+ 模型与 onnxruntime 依赖，增加包体积与维护复杂度；转为未来第二阶段可选离线 Fallback。
  * **Qt QTextToSpeech / Speech Dispatcher**：Linux 下 eSpeak NG 音质过于机械，且无法获取 PCM 数据。
* **理由**：
  * 实测数据表现优异：HTTP 连接复用下热态 TTFB 为 220ms~340ms，RTF 低至 0.099~0.138。
  * 零第三方依赖：仅使用项目现有的 `Qt6::Network` 与 `Qt6::Multimedia`。
  * 免费额度友好：Azure F0 免费层每月提供 50 万字符神经语音额度。

---

### 决定 2：采用 QAudioSink 本机播放 + OBS 桌面音频捕获作为音频路由

* **选择**：通过 Qt6 `QAudioSink` 直接将 PCM 数据输出到系统默认音频播放设备，由 OBS 现有的“桌面音频（Desktop Audio）”捕获并输出到直播流。
* **排除方案**：
  * **自定义 OBS Audio Source 注入**：需要在 OBS 场景中新增音频源，处理复杂的采样率转换、重采样与时钟同步，开发与使用门槛高。
* **理由**：
  * 架构极其简洁，完全复用主播现有的 OBS 桌面音频设置，主播与观众端体验一致。

---

### 决定 3：独立消费者与单 Worker 异步调度机制

* **选择**：
  * `TtsManager` 作为独立消费者监听 `DanmakuWebSocket` 的 `danmaku_received`、`gift_received`、`super_chat_received` 信号。
  * 维护独立的 `TtsWorker`（或异步任务循环），文本清洗、SSML 构建、网络请求、音频流式播放均在异步任务中串行进行。
* **理由**：
  * 彻底贯彻单一职责与故障隔离原则，TTS 失败或卡顿时，`DanmakuDisplay` 与推流主流程丝毫不受影响。

---

### 决定 4：有界优先级队列与弹幕智能合并策略

* **选择**：
  * 队列上限为 1000 条，已入队消息不静默丢弃。
  * **优先级调度**：`SuperChat` (最高) > `Gift` > `Normal` > `Entry` (进房欢迎最低)。
  * **进房范围过滤**：支持按「全部观众」、「仅佩戴粉丝勋章」与「仅大航海（舰长/提督/总督）」三级范围过滤，防止高频进房打爆限流与积压队列。
  * **智能批量合并**：当队列中有积压的普通短弹幕或进房消息时，自动合并为复合句子发起单次 TTS 合成。
* **理由**：
  * 极大缓解 Azure F0 免费层 20 次/60 秒的请求频次限制；
  * 实测 40+ 字的合并句子 RTF 仅 0.099，听感连贯，大幅提升排队消化速度。

---

### 决定 5：用户自主配置 Key 与 AES 加密持久化

* **选择**：
  * 插件不硬编码任何共享 Key，由主播在设置中配置自己的 Azure Subscription Key 与 Region。
  * 复用 `ConfigManager` 的 AES-128-CBC + HMAC-SHA256 加密机制持久化存储到 `config.json`。
* **理由**：
  * 保障主播账号与额度安全，避免共享 Key 导致的滥用、欠费或泄露风险。

---

## 模块结构与职责

```text
src/tts/
├── tts-types.h            # 数据结构定义（TtsConfig, TtsMessage, TtsPriority）
├── tts-cleaner.h/.cpp     # 弹幕清洗（敏感词/表情/URL过滤）与批量合并器
├── tts-backend.h          # ITtsBackend 纯虚接口
├── azure-tts-backend.h/.cpp # Azure REST API 实现（SSML构建、HTTP流式接收、指标收集）
├── pcm-audio-sink.h/.cpp  # 基于 QAudioSink 的 PCM 播放器（音量控制、打断、清空）
└── tts-manager.h/.cpp     # 顶层管理协调器（队列管理、Worker 调度、生命周期安全）
```

---

## 影响与收益

- **正面收益**：
  - 为 OBS 插件补齐了高自然度的中文弹幕语音播报功能；
  - 零外部二进制依赖，包体积零增长，构建和 CI 完全兼容；
  - 具备优秀的抗压能力（智能合并 + 优先队列 + 故障强隔离）。
- **潜在代价与缓解**：
  - 需要用户申请并配置 Azure Speech Key（在设置界面提供清晰的引导和一键测试试听功能）。
