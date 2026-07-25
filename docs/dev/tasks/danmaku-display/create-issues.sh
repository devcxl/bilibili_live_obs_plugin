# GitHub Issues 创建脚本

# 请在仓库根目录执行（已登录 gh CLI）

# ── 1. 创建 Parent Issue #6 ──
cat > /tmp/issue-6-body.md << 'EOF'
## 技术方案

`docs/dev/specs/danmaku-display.md`

## ADR

`docs/adr/2026-07-25-danmaku-websocket.md`

## 需求概要

在 OBS 插件的推流线路 UI 下方添加实时弹幕展示区域。

### 核心流程
1. 开播后自动获取弹幕 WebSocket Token（HTTP + WBI 签名）
2. 建立 WSS 连接 → 认证 → 心跳维持（30s）
3. 解析二进制消息 → Brotli 解压 → 弹幕/礼物/SC 类型分发
4. QListWidget 实时展示弹幕（最新在顶部，最多 500 条）
5. 停播后自动断开 WebSocket 连接 + 隐藏弹幕面板

### 新增模块
- `src/danmaku-ws.h/.cpp` — WebSocket 客户端核心
- `src/danmaku-display.h/.cpp` — Qt6 弹幕展示面板
- `src/bilibili-api.h/.cpp` — 新增 `get_danmu_info()` HTTP API
- `CMakeLists.txt` — 新增 libbrotli 依赖

### 设计原则
- 弹幕功能与核心开播功能完全隔离，任何异常不影响推流
- 消息通过 Qt 信号/槽线程安全传递
- 固定大小环形缓冲区（1000 条），低内存占用
- 指数退避重连（1s→2s→4s→...→max 30s）
EOF

gh issue create \
  --title "feat: 推流线路下方添加实时弹幕展示" \
  --label "enhancement" \
  --body-file /tmp/issue-6-body.md

# ── 2. 创建 Sub Issue #7: Task 1 ──
cat > /tmp/issue-7-body.md << 'EOF'
## 依赖
无

## Worktree
- 路径: `.worktree/task-01-cmake-and-structs/`
- 分支: `feat/task-01-cmake-and-structs`

## 描述
在 CMakeLists.txt 中添加 libbrotli 编译依赖，注册 danmaku-ws.cpp 和 danmaku-display.cpp 两个新源文件，创建 danmaku-ws.h 定义 DanmakuMessage / GiftMessage / SuperChatMessage 结构体及 Q_DECLARE_METATYPE 注册。

## 涉及文件
- `CMakeLists.txt` — libbrotli 依赖 + 源文件注册 + link + CPack 依赖
- `src/danmaku-ws.h`（新建）— 消息数据结构定义

## 详细步骤
参考: `docs/dev/tasks/danmaku-display/task-01-cmake-and-structs.md`
EOF

gh issue create \
  --title "task-01-cmake-and-structs: CMakeLists.txt 变更 + 公共数据结构定义" \
  --label "task" \
  --body-file /tmp/issue-7-body.md

# ── 3. 创建 Sub Issue #8: Task 2 ──
cat > /tmp/issue-8-body.md << 'EOF'
## 依赖
task-01-cmake-and-structs

## Worktree
- 路径: `.worktree/task-02-api-get-danmu-info/`
- 分支: `feat/task-02-api-get-danmu-info`

## 描述
在 BilibiliApi 类中新增 get_danmu_info() 方法，通过 HTTP GET + WBI 签名调用 B站 `getDanmuInfo` API，获取 WebSocket 连接所需的 token 和 host_list。

## 涉及文件
- `src/bilibili-api.h` — 新增 get_danmu_info 方法声明
- `src/bilibili-api.cpp` — 新增实现（WBI 签名 + HTTP GET + JSON 解析）

## 详细步骤
参考: `docs/dev/tasks/danmaku-display/task-02-api-get-danmu-info.md`
EOF

gh issue create \
  --title "task-02-api-get-danmu-info: BilibiliApi::get_danmu_info() HTTP API 实现" \
  --label "task" \
  --body-file /tmp/issue-8-body.md

# ── 4. 创建 Sub Issue #9: Task 3 ──
cat > /tmp/issue-9-body.md << 'EOF'
## 依赖
task-01-cmake-and-structs, task-02-api-get-danmu-info

## Worktree
- 路径: `.worktree/task-03-danmaku-ws/`
- 分支: `feat/task-03-danmaku-ws`

## 描述
实现 DanmakuWebSocket 类，完成 B站弹幕 WebSocket 客户端的全部核心逻辑：获取连接参数 → WSS 连接 → 认证 → 心跳维持（30s）→ 二进制包解析（16字节头 + protover=0/3 分支）→ Brotli 解压 → 消息类型分发（DANMU_MSG / SEND_GIFT / SUPER_CHAT_MESSAGE）→ 指数退避重连（1s→2s→...→max 30s）。

## 涉及文件
- `src/danmaku-ws.h` — 补充 DanmakuWebSocket 类完整定义
- `src/danmaku-ws.cpp`（新建）— 完整实现（~250 行）

## 详细步骤
参考: `docs/dev/tasks/danmaku-display/task-03-danmaku-ws.md`
EOF

gh issue create \
  --title "task-03-danmaku-ws: DanmakuWebSocket 客户端完整实现" \
  --label "task" \
  --body-file /tmp/issue-9-body.md

# ── 5. 创建 Sub Issue #10: Task 4 ──
cat > /tmp/issue-10-body.md << 'EOF'
## 依赖
task-01-cmake-and-structs

## Worktree
- 路径: `.worktree/task-04-danmaku-display/`
- 分支: `feat/task-04-danmaku-display`

## 描述
实现 DanmakuDisplay QWidget 弹幕展示面板，包含弹幕列表（QListWidget，最新在上）、人气值标签、连接状态标签。通过 slot 接收三种消息类型（弹幕/礼物/SC），不同类型使用不同颜色前缀标识，超出 max_visible（500 条）自动裁剪。

## 涉及文件
- `src/danmaku-display.h`（新建）— DanmakuDisplay 类定义
- `src/danmaku-display.cpp`（新建）— UI 布局 + slot 实现 + 裁剪逻辑（~100 行）

## 详细步骤
参考: `docs/dev/tasks/danmaku-display/task-04-danmaku-display.md`
EOF

gh issue create \
  --title "task-04-danmaku-display: DanmakuDisplay UI 控件实现" \
  --label "task" \
  --body-file /tmp/issue-10-body.md

# ── 6. 创建 Sub Issue #11: Task 5 ──
cat > /tmp/issue-11-body.md << 'EOF'
## 依赖
task-01-cmake-and-structs, task-03-danmaku-ws, task-04-danmaku-display

## Worktree
- 路径: `.worktree/task-05-integration/`
- 分支: `feat/task-05-integration`

## 描述
将所有模块串联：plugin-main.cpp 中创建 DanmakuWebSocket 实例并注入到 BiliDock；BiliDock 中集成 DanmakuDisplay UI 组件（推流线路下方），绑定 4 个信号/槽（danmaku/gift/sc/connection_state）；实现开播后自动连接弹幕 WebSocket + 显示面板，停播后自动断开 + 隐藏面板。

## 涉及文件
- `src/plugin-main.cpp` — 创建/销毁 DanmakuWebSocket，传递到 BiliDock
- `src/bili-dock.h` — 新增 danmaku_ws_ / danmaku_display_ 成员 + set_danmaku_ws() 方法
- `src/bili-dock.cpp` — init_ui() 添加 DanmakuDisplay + 信号绑定 + 开播/停播联动

## 详细步骤
参考: `docs/dev/tasks/danmaku-display/task-05-integration.md`
EOF

gh issue create \
  --title "task-05-integration: plugin-main.cpp + BiliDock 总装集成" \
  --label "task" \
  --body-file /tmp/issue-11-body.md

# ── 7. 关联 Sub Issues 到 Parent #6 ──
for num in 7 8 9 10 11; do
  gh issue edit $num --add-parent 6
done

echo "Done! Issues #6-#11 created."
