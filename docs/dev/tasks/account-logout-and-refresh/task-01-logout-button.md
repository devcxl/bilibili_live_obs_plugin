---
name: "task-01-logout-button"
depends_on: []
labels: ["frontend"]
worktree_root: ".worktree/task-01-logout-button/"
tdd:
  mode: manual
  min_cycles: 1
acceptance:
  - criteria: "登录后右上角显示'退出登录'按钮"
    verification_type: manual
  - criteria: "未登录时不显示退出按钮"
    verification_type: manual
  - criteria: "单账号和多账号均显示退出按钮（而非仅多账号）"
    verification_type: manual
  - criteria: "点击退出后 cookie 清除、UI 回到未登录状态、状态栏显示'已登出'"
    verification_type: manual
  - criteria: "按钮样式与 OBS 深色主题一致（橙色边框+hover 反色）"
    verification_type: manual
  - criteria: "旧 btn_logout_ 的显示逻辑已从 refresh_account_list() 中移除"
    verification_type: manual
---

# Task 1: 右上角退出登录按钮

## 目标概述

在 Dock 面板右上角（与标题同行、右对齐）新增"退出登录"按钮，替代原有仅在多账号时显示的 `btn_logout_`。登录后任何场景下都显示，点击后清除登录态并回到未登录界面。

## 涉及文件

| 文件 | 修改内容 | 变更量 |
|------|----------|--------|
| `src/bili-dock.h` | 新增成员 `QPushButton *btn_header_logout_` | +1 行 |
| `src/bili-dock.cpp` | `init_ui()` 重构标题行、`on_login_done()`/`set_logged_out()` 控制显示、`do_logout()` uid 来源调整、`refresh_account_list()` 删除旧逻辑 | ~30 行 |

## 实现步骤

### 步骤 1：`bili-dock.h` — 新增成员变量

在 `QPushButton *btn_logout_;` 附近添加：

```cpp
QPushButton *btn_header_logout_ = nullptr;  // 右上角退出按钮
```

### 步骤 2：`bili-dock.cpp:init_ui()` — 重构标题行布局

**当前结构**（约 180-183 行）：
```cpp
auto *title_label = new QLabel("B站直播工具 - OBS 插件");
title_label->setAlignment(Qt::AlignCenter);
title_label->setStyleSheet("...");
main_layout->addWidget(title_label);
```

**改为**：
```cpp
// 标题行：标题左对齐 + 退出按钮右对齐
auto *title_row = new QHBoxLayout();
auto *title_label = new QLabel("B站直播工具 - OBS 插件");
title_label->setStyleSheet("...");
title_row->addWidget(title_label);

title_row->addStretch();

btn_header_logout_ = new QPushButton("退出登录");
btn_header_logout_->setStyleSheet(
    "QPushButton { color:#FFB74D; background:transparent; border:1px solid #FFB74D; "
    "border-radius:4px; padding:2px 12px; font-size:12px; }"
    "QPushButton:hover { background:#FFB74D; color:#1e1e1e; }");
btn_header_logout_->setFixedHeight(24);
btn_header_logout_->hide();
connect(btn_header_logout_, &QPushButton::clicked, this, &BiliDock::do_logout);
title_row->addWidget(btn_header_logout_);

main_layout->addLayout(title_row);
```

参考：技术方案 1.1 节。

### 步骤 3：`bili-dock.cpp:on_login_done()` — 登录后显示按钮

在 `on_login_done()` 的 UI 更新部分末尾添加：

```cpp
btn_header_logout_->show();
```

参考：技术方案 1.2 节。

### 步骤 4：`bili-dock.cpp:set_logged_out()` — 登出后隐藏按钮

在 `set_logged_out()` 末尾添加：

```cpp
btn_header_logout_->hide();
```

参考：技术方案 1.2 节。

### 步骤 5：`bili-dock.cpp:do_logout()` — uid 获取调整

将 uid 获取从 `account_combo_->currentData()` 改为 `cfg_->current_uid`：

```cpp
void BiliDock::do_logout()
{
    std::string uid = cfg_->current_uid;  // 优先使用配置层
    if (uid.empty() || !user_) return;

    user_->logout(uid);
    account_combo_->setVisible(account_combo_->count() > 0);
    set_logged_out();
    btn_login_->show();
    status_bar_->setText("已登出");
}
```

参考：技术方案 1.5 节。

### 步骤 6：`bili-dock.cpp:refresh_account_list()` — 删除旧按钮逻辑

从 `refresh_account_list()` 中移除 `btn_logout_` 的显示/隐藏逻辑（原 `btn_logout_->setVisible(count > 1)`）。

参考：技术方案 1.3 节。

### 步骤 7：`bili-dock.h` — 移除旧成员（可选）

如果 `btn_logout_` 已无引用，从 `bili-dock.h` 中移除 `QPushButton *btn_logout_` 声明。

## 旧按钮处理说明

- `btn_logout_`：**废弃移除**。右上角按钮覆盖单/多账号全部场景，旧按钮功能冗余。
- `account_combo_`：**保留**。用于多账号切换 UI，但不显示配套的登出按钮。

参考：技术方案 1.3 节。

## 验收

参考 frontmatter `acceptance` 字段中的 6 条验收标准。
