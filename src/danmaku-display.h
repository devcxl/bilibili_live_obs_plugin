#pragma once

#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QColor>

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
    void update_popularity(int count);
    void update_connection_state(bool connected);
    void clear_all();

private:
    void setup_ui();
    void append_item(const QString &prefix, const QString &text, const QColor &color);
    void trim_list();

    QListWidget *list_widget_;
    QLabel *popularity_label_;
    QLabel *status_label_;
    int max_visible_items_ = 200;
};
