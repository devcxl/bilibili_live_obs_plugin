#include "danmaku-display.h"

#include <QAbstractItemView>

DanmakuDisplay::DanmakuDisplay(QWidget *parent)
    : QWidget(parent)
{
    qRegisterMetaType<DanmakuMessage>("DanmakuMessage");
    qRegisterMetaType<GiftMessage>("GiftMessage");
    qRegisterMetaType<SuperChatMessage>("SuperChatMessage");

    setup_ui();
}

void DanmakuDisplay::setup_ui()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    status_label_ = new QLabel(QStringLiteral("未连接"), this);
    status_label_->setStyleSheet("color: #888888; font-weight: bold;");
    layout->addWidget(status_label_);

    popularity_label_ = new QLabel(QStringLiteral("人气: 0"), this);
    popularity_label_->setStyleSheet("color: #AAAAAA;");
    layout->addWidget(popularity_label_);

    list_widget_ = new QListWidget(this);
    list_widget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_widget_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_widget_->setMinimumHeight(120);
    list_widget_->setMaximumHeight(200);
    list_widget_->setStyleSheet("background: transparent; border: none;");
    layout->addWidget(list_widget_);

    setMinimumHeight(200);
}

void DanmakuDisplay::append_danmaku(const DanmakuMessage &msg)
{
    QString username = QString::fromStdString(msg.username);
    QString message = QString::fromStdString(msg.message);
    QString text;

    if (!msg.fan_badge.empty() && msg.fan_badge_level > 0) {
        text = QStringLiteral("[%1|Lv.%2] %3: %4")
            .arg(QString::fromStdString(msg.fan_badge))
            .arg(msg.fan_badge_level)
            .arg(username)
            .arg(message);
    } else {
        text = QStringLiteral("%1: %2").arg(username).arg(message);
    }

    append_item(QStringLiteral("[弹幕]"), text, QColor("#1E90FF"));
}

void DanmakuDisplay::append_gift(const GiftMessage &msg)
{
    QString text = QStringLiteral("%1 %2 %3 x%4")
        .arg(QString::fromStdString(msg.username))
        .arg(QString::fromStdString(msg.action))
        .arg(QString::fromStdString(msg.gift_name))
        .arg(msg.num);

    if (msg.combo_num > 1) {
        text += QStringLiteral(" (连送%1次)").arg(msg.combo_num);
    }

    append_item(QStringLiteral("[礼物]"), text, QColor("#FFD700"));
}

void DanmakuDisplay::append_super_chat(const SuperChatMessage &msg)
{
    double yuan = msg.price / 100.0;
    QString prefix = QStringLiteral("[SC ¥%1]").arg(yuan, 0, 'f', 2);
    QString text = QStringLiteral("%1: %2")
        .arg(QString::fromStdString(msg.username))
        .arg(QString::fromStdString(msg.message));

    append_item(prefix, text, QColor("#FF4444"));
}

void DanmakuDisplay::update_popularity(int count)
{
    popularity_label_->setText(QStringLiteral("人气: %1").arg(count));
}

void DanmakuDisplay::update_connection_state(bool connected)
{
    if (connected) {
        status_label_->setText(QStringLiteral("已连接"));
        status_label_->setStyleSheet("color: #00CC00; font-weight: bold;");
    } else {
        status_label_->setText(QStringLiteral("未连接"));
        status_label_->setStyleSheet("color: #888888; font-weight: bold;");
    }
}

void DanmakuDisplay::clear_all()
{
    list_widget_->clear();
}

void DanmakuDisplay::set_max_visible_items(int count)
{
    max_visible_items_ = std::max(50, std::min(count, 1000));
    trim_list();
}

void DanmakuDisplay::append_item(const QString &prefix, const QString &text, const QColor &color)
{
    auto *item = new QListWidgetItem(prefix + QStringLiteral(" ") + text);
    item->setForeground(color);
    list_widget_->addItem(item);
    list_widget_->scrollToBottom();
    trim_list();
}

void DanmakuDisplay::trim_list()
{
    while (list_widget_->count() > max_visible_items_) {
        delete list_widget_->takeItem(0);
    }
}
