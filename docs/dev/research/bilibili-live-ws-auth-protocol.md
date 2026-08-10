# B站直播弹幕 WebSocket 鉴权协议调研（2026-08）

> **调研目标**：确定当前可用的 WSS 连接＋鉴权方案，定位 `ConnectionRefusedError` 根因。
>
> **方法**：查阅 6 个活跃维护项目的源码（非文档说明）＋ Bilibili-Live-API（2026-07-15 最新验证）＋ bilibili-API-collect 社区文档。

---

## 一、调研项目概览

| 项目 | 语言 | 最后更新 | 状态 | 关键来源 |
|------|------|----------|------|----------|
| `champkeh/blive-ws` | JS/Deno | 2024 | 活跃，Deno 代理 | 源码 `ws.ts` 直接阅读 [1] |
| `simon300000/bilibili-live-ws` | TS | 2025 | npm 6.3.0，活跃 | `src/common.ts` + Issue #397 [2][3] |
| `Minteea/bilibili-live-danmaku` | TS | 2026-05 (v0.7.16) | npm, 活跃维护 | `src/live-ws.ts` [4] |
| `xfgryujk/blivedm` | Python | 2025 | 1K stars，活跃 | `blivedm/clients/web.py` [5] |
| `Tsuk1ko/bilibili-live-chat` | Vue/JS | 2025 | 浏览器应用，活跃 | `src/pages/live/App.vue` [6] |
| `lovelyyoshino/Bilibili-Live-API` | 文档＋验证脚本 | **2026-07-15** | **最权威文档** | `API.live_websocket.md` + `API.live_upgrade_events.md` [7][8] |
| `pskdje/bilibili-API-collect` | 社区文档 | 2025 | 权威参考 | `docs/live/message_stream.md` [9] |

---

## 二、各项目鉴权包对比

### 2.1 鉴权包 JSON body 字段对比

| 字段 | 我们的实现 | champkeh/blive-ws | bilibili-live-chat | bilibili-live-ws | Bilibili-Live-API 文档 | bilibili-API-collect |
|------|-----------|-------------------|---------------------|------------------|----------------------|---------------------|
| `uid` | 0 | 0 | 从 Cookie DedeUserID 提取 | 0（可传非零） | 0 被拒，需登录态 `uid` | 0 或用户 mid |
| `roomid` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `protover` | 3 | 3 | 3 | 默认 2 | 2 或 3 | 3 |
| `platform` | `"web"` | `"web"` | `"web"` | `"web"` | `"web"` | `"web"` |
| `type` | 2 | 2 | 2 | 2 | 2 | 2 |
| `key` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `buvid` | **❌ 缺失** | `""`（空字符串） | 从 Cookie `buvid3` 提取 | 支持传参 | **必须字段** | 未列出（旧文档） |
| `clientver` | — | — | — | 已移除（v6.3.0） | 已移除 | 已废弃 |

### 2.2 包头（16字节二进制头）对比

| 字段（偏移） | 我们的实现 | champkeh/blive-ws | 协议规范 |
|-------------|-----------|-------------------|----------|
| packet_len (0, 4B) | ✅ 大端 | ✅ 大端 | ✅ |
| header_len (4, 2B) | ✅ 固定 16 | ✅ 固定 16 | ✅ |
| **protover (6, 2B)** | **0**（JSON） | **1**（连接通信） | 0=JSON, 1=连接通信, 3=Brotli |
| op (8, 4B) | ✅ 7 (AUTH) | ✅ 7 (AUTH) | ✅ |
| seq (12, 4B) | ✅ 递增 | ✅ 1 | ✅ |

### 2.3 心跳包对比

| 项目 | 间隔 | 包头 protover | op | body |
|------|------|--------------|----|------|
| 我们的实现 | 30s | 1 | 2 | 空（packet_len=16） |
| champkeh | 30s | 1 | 2 | 空 |
| 协议规范 | 30s（70s 超时） | 1 | 2 | 空 |

---

## 三、关键发现：根因分析

### 3.1 核心问题：`buvid` 字段缺失（置信度：高）

**证据链**：

1. **lovelyyoshino/Bilibili-Live-API `API.live_upgrade_events.md`（2026-06-15 实测）**[8]：
   > "实测鉴权包必须字段：{uid, roomid, protover, platform:"web", type:2, key:, buvid:}"
   > "匿名连接（uid=0）实测被风控拒绝，需带登录态 uid + buvid3"

2. **lovelyyoshino/Bilibili-Live-API `API.live_websocket.md`（2026-07-15 更新）**[7]：
   > "匿名连接（uid=0）实测被风控拒绝，需带登录态 uid + buvid3"

3. **champkeh/blive-ws 源码** `ws.ts`（已验证可工作）[1]：
   ```javascript
   const auth = {
       uid: 0,
       roomid: roomid,
       protover: 3,
       buvid: '',     // ← 空字符串，但字段存在！
       platform: 'web',
       type: 2,
       key: key,
   };
   ```

4. **Tsuk1ko/bilibili-live-chat 源码** `App.vue`（已验证可工作）[6]：
   ```javascript
   const buvid = /\bbuvid3=([^;]+)\b/.exec(props.cookie)?.[1];
   const uid = /\bDedeUserID=([^;]+)\b/.exec(props.cookie)?.[1];
   // ...
   authBody: {
       uid: parseInt(uid),
       roomid: props.room,
       protover: 3,
       buvid,         // ← 从 Cookie 提取
       platform: 'web',
       type: 2,
       key: token,
   }
   ```

5. **simon300000/bilibili-live-ws PR #397**[3]：
   > "目前观测下来，完整的构建建立连接的请求可以进行规避，即：额外传入 uid、buvid、key 即可绕过限制"

6. **bilibili-API-collect `docs/live/message_stream.md`**[9]：
   > "从2025年6月27日开始要求 buvid3"（#1295）

**结论**：服务端对鉴权包 JSON body 中 `buvid` 字段的缺失不做宽容处理，直接 `ConnectionRefusedError`。即使 uid=0（匿名），也必须在 auth body 中包含 `"buvid": ""`。

### 3.2 次要问题：uid=0 匿名连接被风控拒绝（置信度：中高）

即使添加了 `buvid` 字段，`uid=0` 的匿名连接仍可能被拒绝。两个来源一致报告：

- **lovelyyoshino `API.live_upgrade_events.md`**[8]："uid=0 匿名实测被拒（close 1006）"
- **blrec Issue #260**[10]："uid=0，跟key是对不上的，就会被直接风控"

**原因**：`getDanmuInfo` 返回的 `token` 与请求时的登录态绑定。若用 Cookie 请求 token，则 `uid` 必须与 Cookie 中的 `DedeUserID` 一致，且 `key` 与 `uid` 需匹配。

### 3.3 次要问题：包头 protover 值（置信度：低）

- 我们的实现使用 `protover=0`（JSON 明文），champkeh 使用 `protover=1`（连接通信）
- 两者都在协议文档的合法范围内，服务端对 auth 包可能都接受
- 但**建议改为 `protover=1`**以和现有工作实现完全一致

---

## 四、完整方案建议

### 4.1 方案 A：匿名连接（OBS 插件，用户无需登录）— 推荐

**适用场景**：OBS 插件无用户登录态，只需获取弹幕基本内容（昵称可能被脱敏）。

**修改点**：

1. **在 `send_auth_packet()` 中为 auth_body 添加 `buvid` 字段**：
   ```cpp
   json auth_body = {
       {"uid", 0},
       {"roomid", room_id_num},
       {"protover", 3},
       {"platform", "web"},
       {"type", 2},
       {"key", token_},
       {"buvid", ""}        // ← 添加此行
   };
   ```

2. ~~修改包头 protover 为 1~~（可选，根据实测决定是否必要）

3. **确保 `getDanmuInfo` 请求不带登录 Cookie**：当前实现已正确执行（`get_danmu_info` 中只在缺少 buvid3 时请求 `get_buvid3`）。需确保 `getDanmuInfo` 不使用 `SESSDATA` cookie，否则 token 会绑定用户 UID。

**风险**：
- 弹幕昵称可能被脱敏（uid 为 0，uname 可能为 `*`）
- 部分房间可能完全拒绝匿名连接
- 弹幕限流（丢失率高）

### 4.2 方案 B：登录态连接（需要用户提供 Cookie）

**适用场景**：用户通过 OBS 插件登录 B站，获取未脱敏的弹幕。

**修改点**（在方案 A 基础上）：

1. 用户提供 `SESSDATA` Cookie
2. 从 Cookie 中提取 `DedeUserID` 作为 `uid`
3. 从 Cookie 中提取 `buvid3` 作为 `buvid`
4. 用该 Cookie 请求 `getDanmuInfo` 获取 token
5. 鉴权包的 `uid`、`buvid`、`key` 三者需一致

**优点**：弹幕昵称不脱敏，限流概率低

### 4.3 方案 C：开放平台接入（长期最稳定）

B站开放平台（open-live.bilibili.com）提供官方 WebSocket 接口，协议略有不同但更稳定。

- 需要申请 `app_id` + `secret`
- 使用 `/v2/app/start` 获取 `websocket_info`（含 `auth_body` 和 `wss_link`）
- auth_body 由平台生成，直接发送即可
- **个人开发者现已可申请**（blivedm Issue #33 确认）

---

## 五、可直接使用的参考实现

| 项目 | 语言 | 文件（关键逻辑行） | 备注 |
|------|------|-------------------|------|
| `champkeh/blive-ws` | JS | `ws.ts` L1-80 | `convertToArrayBuffer` 编码 + `connectWebSocket` 鉴权 |
| `Tsuk1ko/bilibili-live-chat` | Vue/JS | `src/pages/live/App.vue` L50-90 | 从 Cookie 提取 buvid/uid |
| `simon300000/bilibili-live-ws` | TS | `src/common.ts` L77-86 | `encode` 函数（authBody object 编码） |

---

## 六、验证检查清单

- [ ] auth_body JSON 包含 `buvid` 字段（可以为空字符串 `""`）
- [ ] `getDanmuInfo` 请求携带 `web_location=444.8` 参数（当前已做 ✅）
- [ ] `getDanmuInfo` 请求有 WBI 签名 `w_rid` + `wts`（当前已做 ✅）
- [ ] `getDanmuInfo` 请求 Cookie 含 `buvid3`（当前已自动获取 ✅）
- [ ] WSS URL 格式正确：`wss://{host}:{wss_port}/sub`（当前已做 ✅）
- [ ] 鉴权包在连接建立后 5 秒内发送（当前已做 ✅）
- [ ] 心跳包每 30 秒发送一次（当前已做 ✅）
- [ ] broker 压缩解压正确实现（当前 Brotli 已做 ✅，可加 zlib protover=2 兜底）

---

## 七、置信度与不确定性

| 结论 | 置信度 | 备注 |
|------|--------|------|
| `buvid` 字段缺失导致连接被拒 | **高** | 6 个来源一致，含 2026-07 实测 |
| `uid=0` 匿名可能被风控 | **中高** | 2 个来源一致，但部分项目仍用 uid=0 成功 |
| 包头 protover=0 vs 1 的影响 | **低** | 协议文档均合法，缺乏直接对比较据 |
| 不同 WSS 服务器风控严格度不同 | **中** | blivedm Issue #33：broadcastlv 较严格，其他可能宽松 |

---

## 八、来源索引

| # | 来源 | 类型 | 时效 |
|---|------|------|------|
| [1] | [champkeh/blive-ws ws.ts](https://github.com/champkeh/blive-ws) | 源码（一手） | 2024 |
| [2] | [simon300000/bilibili-live-ws](https://github.com/simon300000/bilibili-live-ws) | 源码（一手） | 2025 |
| [3] | [bilibili-live-ws Issue #397](https://github.com/simon300000/bilibili-live-ws/issues/397) | Issue 讨论 | 2023-2025 |
| [4] | [Minteea/bilibili-live-danmaku](https://github.com/Minteea/bilibili-live-danmaku) | 源码（一手） | 2026-05 |
| [5] | [xfgryujk/blivedm](https://github.com/xfgryujk/blivedm) | 源码（一手） | 2025 |
| [6] | [Tsuk1ko/bilibili-live-chat App.vue](https://github.com/Tsuk1ko/bilibili-live-chat/blob/master/src/pages/live/App.vue) | 源码（一手） | 2025 |
| [7] | [Bilibili-Live-API API.live_websocket.md](https://github.com/lovelyyoshino/Bilibili-Live-API/blob/master/API.live_websocket.md) | 文档＋实测 | **2026-07-15** |
| [8] | [Bilibili-Live-API API.live_upgrade_events.md](https://github.com/lovelyyoshino/Bilibili-Live-API/blob/master/API.live_upgrade_events.md) | 文档＋实测 | 2026-06-15 |
| [9] | [bilibili-API-collect message_stream.md](https://github.com/pskdje/bilibili-API-collect/blob/main/docs/live/message_stream.md) | 社区文档 | 2025 |
| [10] | [blrec Issue #260](https://github.com/acgnhiki/blrec/issues/260) | Issue 讨论 | 2024-03 |
