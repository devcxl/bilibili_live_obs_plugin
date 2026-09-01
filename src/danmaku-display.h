#pragma once

#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>

#include "danmaku-ws.h"

class DanmakuDisplay : public QWidget {
    Q_OBJECT

public:
    explicit DanmakuDisplay(QWidget *parent = nullptr);
    void set_max_visible_items(int count);

public slots:
    void append_danmaku(const DanmakuMessage &msg);
    void append_gift(const GiftMessage &msg);
    void append_super_chat(const SuperChatMessage &msg);
    void set_popularity(int popularity);
    void set_connected(bool connected);
    void set_status_text(const QString &text, const QString &style = "");
    void clear_all();

signals:
    void reconnect_requested();

private:
    void trim_items();

    QListWidget *list_widget_;
    QLabel *popularity_label_;
    QLabel *status_label_;
    QPushButton *btn_reconnect_;
    int max_visible_ = 200;
};
