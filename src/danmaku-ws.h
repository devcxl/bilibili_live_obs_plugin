#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QTimer>
#include <QWebSocket>
#include <QAbstractSocket>
#include <QSslError>

#include "bilibili-api.h"
#include "config-manager.h"
#include "danmaku/danmaku-packet.h"

// ─── 弹幕消息 ───
struct DanmakuMessage {
    std::string cmd;           // "LIVE_OPEN_PLATFORM_DM"
    std::string username;      // 发送者昵称
    std::string uid;           // 发送者 OpenID
    std::string message;       // 弹幕文本
    std::string fan_badge;     // 粉丝勋章名（可选）
    int fan_badge_level = 0;   // 粉丝勋章等级
    int guard_level = 0;       // 大航海等级：0=无，1=总督，2=提督，3=舰长
};

// ─── 礼物消息 ───
struct GiftMessage {
    std::string username;      // 送礼者昵称
    std::string gift_name;     // 礼物名称
    int num = 0;               // 数量
    int combo_num = 0;         // 连送次数（combo）
    std::string action;        // "赠送" / "开通" / "续费"
};

// ─── SC 醒目留言 ───
struct SuperChatMessage {
    std::string username;      // 发送者昵称
    std::string message;       // SC 内容
    int price = 0;             // 金额（人民币，单位：分）
};

// ─── 进房与互动消息 ───
struct EntryMessage {
    std::string username;      // 观众昵称
    std::string uid;           // 观众 OpenID
    int guard_level = 0;       // 大航海等级：0=无，1=总督，2=提督，3=舰长
    std::string medal_name;    // 粉丝勋章名
    int medal_level = 0;       // 粉丝勋章等级
    int msg_type = 1;          // 1: 进入房间, 2: 关注, 3: 分享
};

// 注册到 Qt 元对象系统（支持跨线程信号/槽值类型传递）
Q_DECLARE_METATYPE(DanmakuMessage)
Q_DECLARE_METATYPE(GiftMessage)
Q_DECLARE_METATYPE(SuperChatMessage)
Q_DECLARE_METATYPE(EntryMessage)

// ─── B站开放平台 (Open Live) 官方长连接客户端 ───
class DanmakuWebSocket : public QObject {
    Q_OBJECT
public:
    explicit DanmakuWebSocket(QObject *parent = nullptr);
    ~DanmakuWebSocket() override;

    // 依赖注入
    void set_api(BilibiliApi *api);
    void set_config(ConfigManager *cfg);

    // 生命周期管理
    void connect_to_room(const std::string &room_id = "");
    void disconnect_from_room();
    [[nodiscard]] bool is_connected() const;
    [[nodiscard]] int popularity() const;

signals:
    void danmaku_received(const DanmakuMessage &msg);
    void gift_received(const GiftMessage &msg);
    void super_chat_received(const SuperChatMessage &msg);
    void entry_received(const EntryMessage &msg);
    void connection_state_changed(bool connected, int popularity);

private slots:
    void on_ws_connected();
    void on_ws_disconnected();
    void on_ws_binary_message(const QByteArray &data);
    void on_ws_error(QAbstractSocket::SocketError error);
    void on_ws_ssl_errors(const QList<QSslError> &errors);
    void send_heartbeat();
    void send_open_http_heartbeat();
    void attempt_reconnect();

private:
    void send_auth_packet();
    void start_heartbeat();
    void stop_heartbeat();
    void start_reconnect();
    void stop_reconnect();

    void connect_async(uint64_t gen);

    // 成员变量
    QWebSocket *ws_ = nullptr;
    QTimer *heartbeat_timer_ = nullptr;
    QTimer *open_heartbeat_timer_ = nullptr;
    QTimer *reconnect_timer_ = nullptr;
    BilibiliApi *api_ = nullptr;
    ConfigManager *cfg_ = nullptr;

    std::thread fetch_thread_;

    std::string room_id_;
    std::string open_game_id_;
    std::string open_auth_body_;

    uint32_t seq_ = 1;
    int popularity_ = 0;
    bool authenticated_ = false;
    bool intentional_disconnect_ = false;

    // 连接代次：每次 connect/disconnect 递增
    std::atomic<uint64_t> connect_gen_{0};

    int reconnect_attempts_ = 0;
    static constexpr int RECONNECT_BASE_DELAY_MS = 1000;
    static constexpr int RECONNECT_MAX_DELAY_MS = 30000;
};
