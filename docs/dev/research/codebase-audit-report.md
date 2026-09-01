# 代码审查与架构缺陷评估报告

- **审查日期**：2025-05-18
- **审查范围**：全仓库源码（`src/` 下 12 个源文件，共 3,588 行 C++ 代码）及构建系统
- **目标**：排查崩溃/死锁/UAF、跨线程违规、内存与句柄泄漏、协议兼容性、数据持久化及架构可维护性问题

---

## 目录

- [一、复查结论总览](#一复查结论总览)
- [二、P0 级致命与高危隐患（崩溃 / UAF / 跨线程安全）](#二p0-级致命与高危隐患崩溃--uaf--跨线程安全)
- [三、P1 级业务与性能缺陷（性能瓶颈 / 协议漏洞 / 边界异常）](#三p1-级业务与性能缺陷性能瓶颈--协议漏洞--边界异常)
- [四、P2 级架构设计与可维护性评估](#四p2-级架构设计与可维护性评估)
- [五、修复实施规划与 PR 拆解 (DAG)](#五修复实施规划与-pr-拆解-dag)

---

## 一、复查结论总览

经过对全量源码的二次严格复查与调用链路追踪，共确认 **9 项确凿隐患**：
- **P0 级（致命/线程安全）**：3 项（弹幕异步 UAF 崩溃、OBS 回调跨线程操作 UI、CURL 共享句柄全局锁阻塞 UI）
- **P1 级（性能/协议/数据）**：4 项（弹幕高频 setItemWidget 性能卡顿、缺失 zlib 压缩协议、扫码后同步多重阻塞、配置保存非原子性）
- **P2 级（代码规范/架构）**：2 项（PBKDF2 加密注释与实现不一致、Fat Widget 与缺乏测试）

---

## 二、P0 级致命与高危隐患（崩溃 / UAF / 跨线程安全）

### 1. [P0-1] `DanmakuWebSocket` 异步任务引发 Use-After-Free (UAF) 与退出卡死

- **涉及文件**：`src/danmaku-ws.cpp` (75–95行), `src/plugin-main.cpp` (52–62行 `destroy_services`)
- **代码定位**：
  ```cpp
  // danmaku-ws.cpp
  const uint64_t gen = ++connect_gen_;
  BilibiliApi *api = api_;
  auto future = std::async(std::launch::async, [api, room_id]() {
      return api->get_danmu_info(room_id);
  });
  poll_danmu_info_future(std::move(future), gen);
  ```
- **根因分析**：
  1. **UAF 致命崩溃**：当 OBS 退出或执行注销时，`plugin-main.cpp` 的 `destroy_services()` 会按序 `delete s_danmaku_ws; ... delete s_api;`。若此时后台 `std::async` 线程正在执行网络 I/O（`curl_easy_perform` 超时长达 10s），该线程捕获的 `api` 裸指针瞬间失效，继续解引用会导致非法内存访问崩溃（SIGSEGV）。
  2. **主线程析构卡死**：C++11 标准规定，未就绪的 `std::future`（由 `std::async` 创建）在析构时会隐式同步阻塞等待线程结束。当 `s_danmaku_ws` 析构时，挂在 Qt 事件循环里的定时器 lambda 析构会引发主线程长达数秒的无响应冻结。
- **修复方案**：
  废弃 `std::async` + 50ms Timer 轮询的不可控方案。使用基于 `QThread` / `QThreadPool` 的 Worker 模式，或使用 `QPointer` + 生命周期安全 Guard，并在析构时显式等待/取消后台任务。

---

### 2. [P0-2] OBS 推流事件回调非主线程直接操作 Qt 控件

- **涉及文件**：`src/plugin-main.cpp` (116–126行 `on_frontend_event`)
- **代码定位**：
  ```cpp
  static void on_frontend_event(enum obs_frontend_event event, void *)
  {
      if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
          obs_queue_task(OBS_TASK_UI, ui_dock_load, nullptr, false); // ✅ 正确投递到 UI 线程
      }
      if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED && s_dock) {
          s_dock->on_obs_streaming_started(); // ❌ 跨线程直接调用 UI 成员函数
      }
      if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED && s_dock) {
          s_dock->on_obs_streaming_stopped(); // ❌ 跨线程直接调用 UI 成员函数
      }
  ...
  ```
- **根因分析**：
  OBS 前端推流状态事件回调可能由 OBS 底层推流工作线程触发。`on_obs_streaming_started()` / `stopped()` 内部直接修改了 `stream_status_->setText(...)`、`btn_start_->setEnabled(...)` 等 Qt 控件。Qt 严格要求所有 GUI 控件操作必须在主 GUI 线程中执行，跨线程直调会导致未定义行为、随机崩溃或画面撕裂。
- **修复方案**：
  统一通过 `obs_queue_task(OBS_TASK_UI, ...)` 或 `QMetaObject::invokeMethod(s_dock, ..., Qt::QueuedConnection)` 投递至 Qt 主事件循环执行。

---

### 3. [P0-3] 单一共享 `CURL*` 句柄加锁导致 UI 假死与状态污染

- **涉及文件**：`src/bilibili-api.h` (74–76行), `src/bilibili-api.cpp` (160–220行)
- **代码定位**：
  ```cpp
  ApiResult BilibiliApi::do_request(const std::string &method, const std::string &url,
                                     const json &params, const json &data)
  {
      std::lock_guard<std::recursive_mutex> lock(api_mutex_);
      // 使用单一成员变量 curl_
      curl_easy_setopt(curl_, ...);
      ...
      CURLcode cc = curl_easy_perform(curl_); // 阻塞式网络 I/O，超时 10s
  ```
- **根因分析**：
  1. **UI 线程互斥锁死**：`api_mutex_` 在每次 `do_request` 中全程持锁。当后台线程执行 `get_danmu_info` 等耗时操作时，UI 线程任何同步 API 调用（如更新分区、修改标题、刷新账号信息）都将被阻塞在 `api_mutex_` 互斥锁上，造成 OBS 界面卡顿。
  2. **CURL 句柄参数残留污染**：复用同一个 `CURL*` 句柄时未在每次请求前调用 `curl_easy_reset(curl_)`。前一个 POST 请求设置的 `CURLOPT_POSTFIELDS` / `CURLOPT_POSTFIELDSIZE` 可能残留至后续 GET 请求；`CURLOPT_COOKIE` 在注销清空 cookie 后仍可能残留在句柄内部。
- **修复方案**：
  将每个 HTTP 请求设计为使用局部独立的 `CURL*` 句柄（RAII 包装，执行后立即 `curl_easy_cleanup`，或使用连接池），解除 `api_mutex_` 对整个网络 I/O 生命周期的垄断。

---

## 三、P1 级业务与性能缺陷（性能瓶颈 / 协议漏洞 / 边界异常）

### 1. [P1-1] 弹幕列表采用 `setItemWidget` 导致高频弹幕下 CPU 飙升与 UI 掉帧

- **涉及文件**：`src/danmaku-display.cpp` (63–88行 `append_danmaku`)
- **代码定位**：
  ```cpp
  auto *item = new QListWidgetItem();
  auto *label = new QLabel(text);
  label->setTextFormat(Qt::RichText);
  list_widget_->insertItem(0, item);
  list_widget_->setItemWidget(item, label); // ❌ 每个 Item 一个 QWidget
  ```
- **根因分析**：
  `QListWidget::setItemWidget` 为每个弹幕项创建独立的 QWidget 句柄。当直播间弹幕刷屏（每秒数十条）且列表滚动上限达到 200 项时，Qt 窗口系统需要频繁销毁/创建/重排 200 个带 RichText 解析的子部件，造成 CPU 占用激增和严重的 OBS 界面卡顿。
- **修复方案**：
  - 方案 A：使用 `QStyledItemDelegate` 自定义绘制富文本项，Item 仅保留文本与属性数据，消除 Widget 开销。
  - 方案 B：改用 `QTextBrowser` / `QTextEdit` 追加 HTML 行，原生高效支持富文本与自动滚动。

---

### 2. [P1-2] WebSocket 弹幕解析缺失 `protover == 2` (zlib/deflate) 支持

- **涉及文件**：`src/danmaku-ws.cpp` (248–267行)
- **代码定位**：
  ```cpp
  } else if (op == 5) {
      if (protover == 0) {
          process_message(body.toStdString());
      } else if (protover == 3) {
          QByteArray decompressed = brotli_decompress(body);
          ...
      }
      // ❌ 缺失 protover == 2 处理
  }
  ```
- **根因分析**：
  B 站弹幕 WebSocket 协议规范定义了 `protover=0`（明文）、`protover=2`（zlib/deflate 压缩）、`protover=3`（Brotli 压缩）。在部分旧节点或网络降级情况下，服务器会下发 `protover=2` 数据包。当前代码直接静默丢弃，导致弹幕无法显示。
- **修复方案**：
  引入 `zlib` / `qUncompress` 解压逻辑，补充 `protover == 2` 分支。

---

### 3. [P1-3] 扫码登录成功后的瀑布式同步阻塞

- **涉及文件**：`src/auth-service.cpp` (436–470行 `poll_login_status`)
- **根因分析**：
  在 UI 定时器槽函数 `poll_login()` 轮询到扫码成功（`code == 0`）时，`poll_login_status` 内部连续同步串行调用了：
  1. `fetch_room_id()`（1~2 次 HTTP）
  2. `fetch_full_user_data()`（2 次 HTTP）
  3. `save_user_data()`（写磁盘配置）
  4. `live_svc_->refresh_partitions()`（1 次 HTTP）
  合计连续 4~5 次阻塞式 HTTP 请求全部在 UI 主线程执行，耗时可达 1~3 秒，用户扫码确认瞬间界面发生明显“假死”。
- **修复方案**：
  将扫码成功后的账号信息与分区拉取流程下沉到后台异步 Worker，完成后通过信号通知 UI 展示，主线程保持响应。

---

### 4. [P1-4] 配置文件缺少原子写入保护

- **涉及文件**：`src/config-manager.cpp` (260–275行 `ConfigManager::save`)
- **代码定位**：
  ```cpp
  std::ofstream out(config_path());
  if (out.is_open())
      out << j.dump(2);
  ```
- **根因分析**：
  直接覆盖写入 `config.json`。若写文件过程中 OBS 被强制关闭、系统崩溃或断电，会导致配置文件截断为 0 字节或半写状态损坏，用户登录态和历史直播配置永久丢失。
- **修复方案**：
  采用 POSIX 标准的原子写入实践：写入 `config.json.tmp` -> `flush`/`fsync` -> `rename` 覆盖目标文件。

---

## 四、P2 级架构设计与可维护性评估

### 1. [P2-1] 密钥派生算法注释与底层实现不一致
- **涉及文件**：`src/config-manager.cpp` (49–59行 `derive_key`)
- **现状**：注释声明使用 `PBKDF2-HMAC-SHA256`，实际调用的是 `PKCS5_PBKDF2_HMAC_SHA1`。为保证已保存配置文件的向后解密兼容性，应修正代码注释，或在升级版本中引入多版本兼容平滑迁移。

### 2. [P2-2] UI 容器职责过重 (Fat Widget) 与缺乏测试
- **涉及文件**：`src/bili-dock.cpp` (930+ 行)
- **现状**：`BiliDock` 混合了 UI 渲染、网络头像加载（`QNetworkAccessManager`）、OBS 推流配置覆盖与回滚（`obs_frontend_set_streaming_service`）、多线路切换等多种职责；且所有业务逻辑紧耦合了 OBS 运行时日志与 Qt UI，无法脱离 OBS 环境执行自动化单元测试。

---

## 五、修复实施规划与 PR 拆解 (DAG)

按照单一职责与独立可验证原则，推荐拆分为 **4 个独立 PR** 逐步推进修复：

```
PR 1 (P0 核心安全) ───► PR 2 (并发与网络) ───► PR 3 (弹幕协议与渲染) ───► PR 4 (数据持久化与健壮性)
```

| PR 序号 | 任务分支名 | 核心修复内容 | 对应风险项 |
| :--- | :--- | :--- | :--- |
| **PR 1** | `fix/obs-events-and-uaf` | 1. 修复 OBS 前端事件跨线程调用 UI 问题 (`obs_queue_task`)<br/>2. 修复 `DanmakuWebSocket` 异步任务生命周期，消除 UAF 与卡死 | P0-1, P0-2 |
| **PR 2** | `fix/curl-handle-concurrency` | 1. 改造 `BilibiliApi` 消除全局 `curl_` 共享与大粒度锁阻塞<br/>2. 扫码登录成功后的多重 HTTP 请求异步化 | P0-3, P1-3 |
| **PR 3** | `fix/danmaku-perf-and-zlib` | 1. 补充 WebSocket `protover=2` (zlib) 解包支持<br/>2. 重构 `DanmakuDisplay`（消除 `setItemWidget` 性能隐患） | P1-1, P1-2 |
| **PR 4** | `fix/config-atomic-save` | 1. `ConfigManager` 增加原子写入（tmp+rename）<br/>2. 修正密钥派生注释与异常容错 | P1-4, P2-1 |
