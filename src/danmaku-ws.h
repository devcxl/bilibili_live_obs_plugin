#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <future>
#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QTimer>
#include <QWebSocket>
#include <QAbstractSocket>
#include <QSslError>

#include "bilibili-api.h"

// ─── 弹幕消息 ───
struct DanmakuMessage {
    std::string cmd;           // "DANMU_MSG"
    std::string username;      // 发送者昵称
    std::string uid;           // 发送者 UID
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
    std::string action;        // "赠送" / "续费"
};

// ─── SC 醒目留言 ───
struct SuperChatMessage {
    std::string username;      // 发送者昵称
    std::string message;       // SC 内容
    int price = 0;             // 金额（人民币，单位：分）
};

// 注册到 Qt 元对象系统（支持跨线程信号/槽值类型传递）
Q_DECLARE_METATYPE(DanmakuMessage)
Q_DECLARE_METATYPE(GiftMessage)
Q_DECLARE_METATYPE(SuperChatMessage)

// ─── WebSocket 客户端 ───
class DanmakuWebSocket : public QObject {
    Q_OBJECT
public:
    explicit DanmakuWebSocket(QObject *parent = nullptr);
    ~DanmakuWebSocket() override;

    // 依赖注入
    void set_api(BilibiliApi *api);

    // 生命周期
    void connect_to_room(const std::string &room_id);
    void disconnect_from_room();
    [[nodiscard]] bool is_connected() const;
    [[nodiscard]] int popularity() const;

    // 消息缓存
    [[nodiscard]] const DanmakuMessage &cached_message(size_t index) const;
    [[nodiscard]] size_t cached_message_count() const;

signals:
    void danmaku_received(const DanmakuMessage &msg);
    void gift_received(const GiftMessage &msg);
    void super_chat_received(const SuperChatMessage &msg);
    void connection_state_changed(bool connected, int popularity);

private slots:
    void on_ws_connected();
    void on_ws_disconnected();
    void on_ws_binary_message(const QByteArray &data);
    void on_ws_error(QAbstractSocket::SocketError error);
    void on_ws_ssl_errors(const QList<QSslError> &errors);
    void send_heartbeat();
    void attempt_reconnect();

private:
    // 内部方法
    void send_auth_packet();
    void parse_packet_loop(const QByteArray &buffer, int depth = 0);
    void process_message(const std::string &json_str);
    void start_heartbeat();
    void stop_heartbeat();
    void start_reconnect();
    void stop_reconnect();
    QByteArray brotli_decompress(const QByteArray &compressed);
    static QByteArray build_packet_header(uint32_t packet_len, uint16_t protover,
                                           uint32_t op, uint32_t seq);

    // 异步 getDanmuInfo：后台线程执行 HTTP，结果回主线程继续连接
    void poll_danmu_info_future(std::future<ApiResult> future, uint64_t gen);
    void on_danmu_info_ready(uint64_t gen, const ApiResult &res);

    // 成员变量
    QWebSocket *ws_ = nullptr;
    QTimer *heartbeat_timer_ = nullptr;
    QTimer *reconnect_timer_ = nullptr;
    BilibiliApi *api_ = nullptr;

    std::string room_id_;
    std::string token_;
    std::string host_;
    int wss_port_ = 443;

    uint32_t seq_ = 1;
    int popularity_ = 0;
    bool authenticated_ = false;
    bool intentional_disconnect_ = false;

    // 连接代次：每次 connect/disconnect 递增，用于丢弃过期的异步 getDanmuInfo 结果
    uint64_t connect_gen_ = 0;

    int reconnect_attempts_ = 0;
    static constexpr int RECONNECT_BASE_DELAY_MS = 1000;
    static constexpr int RECONNECT_MAX_DELAY_MS = 30000;

    static constexpr size_t CACHE_SIZE = 1000;
    std::vector<DanmakuMessage> message_cache_;
    size_t cache_write_pos_ = 0;
    size_t cache_count_ = 0;
};
