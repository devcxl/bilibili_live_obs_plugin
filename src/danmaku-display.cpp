#include "danmaku-display.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <algorithm>

DanmakuDisplay::DanmakuDisplay(QWidget *parent)
    : QWidget(parent)
{
    qRegisterMetaType<DanmakuMessage>("DanmakuMessage");
    qRegisterMetaType<GiftMessage>("GiftMessage");
    qRegisterMetaType<SuperChatMessage>("SuperChatMessage");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(2);

    // ── 状态栏 ──
    auto *status_row = new QHBoxLayout();
    status_label_ = new QLabel("已断开");
    status_label_->setStyleSheet(
        "color: #888; font-size: 11px; padding: 0 4px;");
    status_row->addWidget(status_label_);

    status_row->addStretch();

    popularity_label_ = new QLabel("人气: 0");
    popularity_label_->setStyleSheet(
        "color: #FFB74D; font-size: 11px; padding: 0 4px;");
    status_row->addWidget(popularity_label_);

    layout->addLayout(status_row);

    // ── 弹幕列表 ──
    list_widget_ = new QListWidget();
    list_widget_->setStyleSheet(
        "QListWidget {"
        "  background: #2b2b2b;"
        "  border: 1px solid #444;"
        "  border-radius: 4px;"
        "  color: #e0e0e0;"
        "  font-size: 12px;"
        "}"
        "QListWidget::item {"
        "  padding: 2px 4px;"
        "  border-bottom: 1px solid #333;"
        "}"
    );
    list_widget_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_widget_->setSelectionMode(QAbstractItemView::NoSelection);
    list_widget_->setFocusPolicy(Qt::NoFocus);
    list_widget_->setMinimumHeight(180);   // 弹幕区最小高度（可由 splitter 拖拽增大）

    layout->addWidget(list_widget_);
}

void DanmakuDisplay::append_danmaku(const DanmakuMessage &msg)
{
    // 昵称颜色：舰长（大航海）红色 > 粉丝团按勋章等级渐变（橘黄→亮金）> 普通用户默认色
    QColor name_color("#e0e0e0");
    if (msg.guard_level > 0) {
        name_color = QColor("#FF5252");   // 舰长：红
    } else if (!msg.fan_badge.empty()) {
        // 勋章等级 1→40，HSL 线性插值：橘黄 #FF9800 → 亮金 #FFD54F
        int level = std::clamp(msg.fan_badge_level, 1, 40);
        const double t = (level - 1) / 39.0;
        const QColor low("#FF9800"), high("#FFD54F");
        name_color = QColor::fromHslF(
            low.hueF() + (high.hueF() - low.hueF()) * t,
            low.saturationF() + (high.saturationF() - low.saturationF()) * t,
            low.lightnessF() + (high.lightnessF() - low.lightnessF()) * t);
    }

    // 富文本：仅昵称着色，消息文本保持默认色（QListWidget item 不支持富文本，用 QLabel item widget）
    QString text = QString("<span style=\"color:%1\">%2</span>: %3")
        .arg(name_color.name(),
             QString::fromStdString(msg.username).toHtmlEscaped(),
             QString::fromStdString(msg.message).toHtmlEscaped());
    auto *item = new QListWidgetItem();
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::RichText);
    label->setStyleSheet("background: transparent; padding: 0 4px;");
    list_widget_->insertItem(0, item);
    list_widget_->setItemWidget(item, label);
    trim_items();
}

void DanmakuDisplay::append_gift(const GiftMessage &msg)
{
    QString text = QString("[礼物] %1 %2 %3%4")
        .arg(QString::fromStdString(msg.username),
             QString::fromStdString(msg.action),
             QString::fromStdString(msg.gift_name))
        .arg(msg.num > 1 ? QString(" x%1").arg(msg.num) : "");
    if (msg.combo_num > 1) {
        text += QString(" (连送%1)").arg(msg.combo_num);
    }
    auto *item = new QListWidgetItem(text);
    item->setForeground(QColor("#4FC3F7"));  // 蓝色
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::append_super_chat(const SuperChatMessage &msg)
{
    double yuan = msg.price / 100.0;
    QString text = QString("[SC ¥%1] %2: %3")
        .arg(yuan, 0, 'f', 2)
        .arg(QString::fromStdString(msg.username),
             QString::fromStdString(msg.message));
    auto *item = new QListWidgetItem(text);
    item->setForeground(QColor("#FFB74D"));  // 金色
    item->setBackground(QColor("#3d2e1a"));  // 深色背景
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::trim_items()
{
    while (list_widget_->count() > max_visible_) {
        delete list_widget_->takeItem(list_widget_->count() - 1);
    }
}

void DanmakuDisplay::set_max_visible_items(int count)
{
    max_visible_ = std::max(50, std::min(count, 1000));
    trim_items();
}

void DanmakuDisplay::set_popularity(int popularity)
{
    popularity_label_->setText(
        QString("人气: %1").arg(popularity));
}

void DanmakuDisplay::set_connected(bool connected)
{
    if (connected) {
        status_label_->setText("已连接");
        status_label_->setStyleSheet(
            "color: #4CAF50; font-size: 11px; padding: 0 4px;");
    } else {
        status_label_->setText("已断开");
        status_label_->setStyleSheet(
            "color: #888; font-size: 11px; padding: 0 4px;");
    }
}

void DanmakuDisplay::clear_all()
{
    list_widget_->clear();
}
