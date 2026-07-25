---
name: "task-02-auto-refresh"
depends_on: []
labels: ["backend"]
worktree_root: ".worktree/task-02-auto-refresh/"
tdd:
  mode: manual
  min_cycles: 1
acceptance:
  - criteria: "OBS 启动后 1-2 秒内用户头像/昵称/等级等自动刷新为最新数据"
    verification_type: manual
  - criteria: "刷新期间 dock 已可见且展示缓存数据（非空白等待）"
    verification_type: manual
  - criteria: "网络异常时（API 返回非 0）无弹窗、无错误提示，UI 保持旧数据"
    verification_type: manual
  - criteria: "刷新成功后 status_bar_ 显示'账户信息已更新'"
    verification_type: manual
  - criteria: "未登录状态时不会触发刷新调用"
    verification_type: manual
---

# Task 2: OBS 启动时自动刷新账户信息

## 目标概述

OBS 启动完成、插件 dock 初始化并恢复缓存会话后，自动异步调用 B站 API 获取最新用户数据（等级、关注、粉丝、硬币等），更新本地缓存和 UI 显示。刷新失败静默降级，不影响正常使用。

## 涉及文件

| 文件 | 修改内容 | 变更量 |
|------|----------|--------|
| `src/bili-dock.h` | 新增 `private slots:` 声明 `refresh_account_info()` | +1 行 |
| `src/bili-dock.cpp` | 新增 `refresh_account_info()` 完整实现 | ~15 行 |
| `src/plugin-main.cpp` | `dock_load()` 末尾添加 QTimer::singleShot 触发刷新 | +4 行 |

## 实现步骤

### 步骤 1：`bili-dock.h` — 新增 slot 声明

在 `private slots:` 区域添加：

```cpp
void refresh_account_info();
```

### 步骤 2：`bili-dock.cpp` — 新增 `refresh_account_info()` 实现

在文件末尾（或其他 slot 实现附近）添加：

```cpp
void BiliDock::refresh_account_info()
{
    if (!user_ || !user_->has_valid_session()) return;

    auto result = user_->refresh_current_user();
    if (result["code"] == 0 && result.contains("data")) {
        QJsonObject data = QJsonDocument::fromJson(
            QByteArray::fromStdString(result["data"].dump())).object();
        update_user_display(data);
        status_bar_->setText("账户信息已更新");
    }
    // code != 0: 静默降级，不更新 UI，不弹错误提示
}
```

参考：技术方案 2.2 节。

### 步骤 3：`plugin-main.cpp:dock_load()` — 添加刷新触发

在 `dock_load()` 末尾的 `check_live_status()` 调用之后添加：

```cpp
// 异步刷新账户信息（延迟触发，不阻塞 dock 加载）
if (s_user->has_valid_session()) {
    QTimer::singleShot(500, s_dock, &BiliDock::refresh_account_info);
}
```

参考：技术方案 2.1/2.2 节。

## 时序设计

```
OBS_FRONTEND_EVENT_FINISHED_LOADING
    → obs_queue_task(OBS_TASK_UI, dock_load)
        → init_services() → init_current_user()   [从本地缓存恢复 session]
        → new BiliDock() → on_login_done(cached_data)  [UI 立即展示缓存数据]
        → obs_frontend_add_dock_by_id()            [dock 挂载到 OBS]
        → check_live_status()
        → QTimer::singleShot(500ms, refresh_account_info)  [延迟触发刷新]
            → UserService::refresh_current_user()  [同步 HTTP，1-2s]
            → update_user_display(new_data)        [更新 UI]
            → status_bar_->setText("账户信息已更新")
```

参考：技术方案 2.1 节。

## 异步策略

选择 **方案 A: QTimer::singleShot 延迟同步调用**。

| 维度 | 说明 |
|------|------|
| 异步程度 | 延迟非阻塞 |
| 复杂度 | 低 |
| 线程安全 | 安全（在 UI 线程中执行） |
| 500ms 延迟 | 确保 dock 和 OBS 界面已完全渲染后再执行 HTTP 调用 |
| 阻塞影响 | HTTP 调用耗时 1-2s，阻塞 UI 线程约 1-2s，但发生在 dock 已加载后 |

备选：如后续网络延迟成为问题，可升级为 `std::async` + `QTimer` 轮询。

参考：技术方案 2.3 节。

## 失败降级策略

| 失败场景 | 用户感知 |
|----------|----------|
| 网络不通 / API 超时 | UI 保持旧缓存数据，无错误提示 |
| API 返回错误码（code ≠ 0） | UI 保持旧缓存数据，无错误提示 |
| 本地缓存不存在 | `has_valid_session() == false`，直接跳过刷新 |
| 用户数据字段缺失 | `update_user_display()` 中已有空值处理（显示 0 或空字符串） |

降级原则：**刷新是增强操作，不是关键路径**。缓存数据即可用，网络的不确定性不应影响主功能。

参考：技术方案 2.4 节。

## 数据流

```
QTimer::singleShot(500ms)
    → BiliDock::refresh_account_info()
        → UserService::has_valid_session()
        → (valid) UserService::refresh_current_user()
            → api_->get_user_info()   [GET x/web-interface/nav]
            → api_->get_user_stat()   [GET x/web-interface/card]
            → cfg_->users[uid] = user_data
            → cfg_->save()
            → return {code: 0, data: ...}
        → (code == 0) update_user_display(data)
            → user_info_label_   ← 昵称 + 等级
            → user_detail_label_ ← UID + 关注/粉丝/硬币/B币
            → user_face_label_   ← 头像图片
```

## 验收

参考 frontmatter `acceptance` 字段中的 5 条验收标准。
