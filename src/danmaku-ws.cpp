#include "danmaku-ws.h"
#include "danmaku/danmaku-packet.h"
#include "danmaku/danmaku-codec.h"
#include "danmaku/danmaku-parser.h"
#include "danmaku/open-live-client.h"

#include <obs-module.h>
#include <QtEndian>
#include <nlohmann/json.hpp>
#include <chrono>
#include <QMetaObject>

using json = nlohmann::json;

DanmakuWebSocket::DanmakuWebSocket(QObject *parent)
    : QObject(parent)
{
    ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    heartbeat_timer_ = new QTimer(this);
    heartbeat_timer_->setSingleShot(true);
    open_heartbeat_timer_ = new QTimer(this);
    open_heartbeat_timer_->setSingleShot(true);
    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);

    connect(ws_, &QWebSocket::connected, this, &DanmakuWebSocket::on_ws_connected);
    connect(ws_, &QWebSocket::disconnected, this, &DanmakuWebSocket::on_ws_disconnected);
    connect(ws_, &QWebSocket::binaryMessageReceived,
            this, &DanmakuWebSocket::on_ws_binary_message);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(ws_, &QWebSocket::errorOccurred, this, &DanmakuWebSocket::on_ws_error);
#else
    connect(ws_, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &DanmakuWebSocket::on_ws_error);
#endif
    connect(ws_, &QWebSocket::sslErrors, this, &DanmakuWebSocket::on_ws_ssl_errors);
    connect(heartbeat_timer_, &QTimer::timeout, this, &DanmakuWebSocket::send_heartbeat);
    connect(open_heartbeat_timer_, &QTimer::timeout, this, &DanmakuWebSocket::send_open_http_heartbeat);
    connect(reconnect_timer_, &QTimer::timeout, this, &DanmakuWebSocket::attempt_reconnect);
}

DanmakuWebSocket::~DanmakuWebSocket()
{
    disconnect_from_room();
    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }
}

void DanmakuWebSocket::set_api(BilibiliApi *api) { api_ = api; }

void DanmakuWebSocket::set_config(ConfigManager *cfg) { cfg_ = cfg; }

bool DanmakuWebSocket::is_connected() const { return authenticated_; }

int DanmakuWebSocket::popularity() const { return popularity_; }

void DanmakuWebSocket::connect_to_room(const std::string &room_id)
{
    // 如果当前已经建立连接且已鉴权成功，直接返回，避免破坏正常长连接
    if (authenticated_) {
        blog(LOG_INFO, "[danmaku-open] already connected, ignoring connect request");
        return;
    }
    // 如果正在连接中，避免并发重复发起 start_app 请求导致触发 7001 冷却期
    if (is_connecting_) {
        blog(LOG_INFO, "[danmaku-open] connection in progress, ignoring duplicate connect request");
        return;
    }

    is_connecting_ = true;
    intentional_disconnect_ = false;
    room_id_ = room_id;
    uint64_t my_gen = ++connect_gen_;

    stop_heartbeat();
    stop_reconnect();

    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }

    blog(LOG_INFO, "[danmaku-open] initiating Open Live official start_app");
    fetch_thread_ = std::thread([this, my_gen]() {
        connect_async(my_gen);
    });
}

void DanmakuWebSocket::connect_async(uint64_t gen)
{
    if (!cfg_) {
        is_connecting_ = false;
        QMetaObject::invokeMethod(this, [this]() {
            emit connection_state_changed(false, 0);
        }, Qt::QueuedConnection);
        return;
    }

    auto res = danmaku::OpenLiveClient::start_app(
        cfg_->danmaku.open_live_app_id,
        cfg_->danmaku.open_live_access_key,
        cfg_->danmaku.open_live_secret,
        cfg_->danmaku.open_live_code
    );

    QMetaObject::invokeMethod(this, [this, gen, res]() {
        if (gen != connect_gen_.load() || intentional_disconnect_) {
            is_connecting_ = false;
            return;
        }

        if (!res.ok) {
            is_connecting_ = false;
            blog(LOG_WARNING, "[danmaku-open] start_app failed: %s (code=%d)",
                 res.msg.c_str(), res.code);
            emit connection_state_changed(false, 0);
            if (res.msg.find("参数不完整") != std::string::npos) {
                return;
            }
            // 遇 7001 冷却期错误，等待 5 秒起步退避重试
            if (res.code == 7001) {
                start_reconnect(5000);
            } else {
                start_reconnect(2000);
            }
            return;
        }

        open_game_id_ = res.game_id;
        open_auth_body_ = res.auth_body;

        if (res.wss_links.empty()) {
            is_connecting_ = false;
            blog(LOG_WARNING, "[danmaku-open] no wss_link returned from official open platform");
            emit connection_state_changed(false, 0);
            start_reconnect(3000);
            return;
        }

        QString wss_url = QString::fromStdString(res.wss_links[0]);
        blog(LOG_INFO, "[danmaku-open] connecting to official websocket %s (game_id=%s)",
             wss_url.toUtf8().constData(), open_game_id_.c_str());

        // 启动官方 HTTP 项目心跳（每 20 秒发送一次）
        open_heartbeat_timer_->start(20000);

        ws_->open(QUrl(wss_url));
    }, Qt::QueuedConnection);
}

void DanmakuWebSocket::disconnect_from_room()
{
    intentional_disconnect_ = true;
    is_connecting_ = false;
    ++connect_gen_;

    stop_heartbeat();
    stop_reconnect();
    open_heartbeat_timer_->stop();

    // 优雅关闭 Open Live 官方项目
    if (!open_game_id_.empty() && cfg_) {
        std::string gid = open_game_id_;
        int64_t aid = cfg_->danmaku.open_live_app_id;
        std::string ak = cfg_->danmaku.open_live_access_key;
        std::string sk = cfg_->danmaku.open_live_secret;
        std::thread([aid, gid, ak, sk]() {
            danmaku::OpenLiveClient::end_app(aid, gid, ak, sk);
        }).detach();
        open_game_id_.clear();
        open_auth_body_.clear();
    }

    if (ws_->state() != QAbstractSocket::UnconnectedState) {
        ws_->abort();
    }

    authenticated_ = false;
    popularity_ = 0;
    room_id_.clear();
    seq_ = 1;
    reconnect_attempts_ = 0;

    emit connection_state_changed(false, 0);
}

void DanmakuWebSocket::on_ws_connected()
{
    blog(LOG_INFO, "[danmaku-open] official websocket connected, sending auth");
    send_auth_packet();
}

void DanmakuWebSocket::on_ws_disconnected()
{
    bool was_auth = authenticated_;
    authenticated_ = false;
    is_connecting_ = false;
    stop_heartbeat();

    blog(LOG_INFO, "[danmaku-open] websocket disconnected (intentional=%d)", intentional_disconnect_);

    if (was_auth) {
        emit connection_state_changed(false, 0);
    }

    if (!intentional_disconnect_) {
        start_reconnect(2000);
    }
}

void DanmakuWebSocket::on_ws_error(QAbstractSocket::SocketError error)
{
    blog(LOG_WARNING, "[danmaku-open] websocket error: %s (code=%d)",
         ws_->errorString().toUtf8().constData(), static_cast<int>(error));
}

void DanmakuWebSocket::on_ws_ssl_errors(const QList<QSslError> &errors)
{
    for (const auto &e : errors) {
        blog(LOG_WARNING, "[danmaku-open] ssl error: %s", e.errorString().toUtf8().constData());
    }
}

void DanmakuWebSocket::send_auth_packet()
{
    if (open_auth_body_.empty()) {
        blog(LOG_WARNING, "[danmaku-open] auth_body empty, aborting auth");
        return;
    }

    blog(LOG_INFO, "[danmaku-open] sending official auth packet");
    QByteArray body = QByteArray::fromStdString(open_auth_body_);
    QByteArray packet = danmaku::DanmakuCodec::encode_packet(
        danmaku::OpCode::Auth,
        danmaku::ProtoVer::Normal,
        body,
        seq_++
    );
    ws_->sendBinaryMessage(packet);
}

void DanmakuWebSocket::on_ws_binary_message(const QByteArray &data)
{
    auto packets = danmaku::DanmakuCodec::decode_packets(data);

    for (const auto &pkt : packets) {
        if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::AuthReply)) {
            auto j = json::parse(pkt.body.toStdString(), nullptr, false);
            if (!j.is_discarded() && j.value("code", -1) == 0) {
                authenticated_ = true;
                blog(LOG_INFO, "[danmaku-open] auth success! listening for official live events");
                start_heartbeat();
                stop_reconnect();
                reconnect_attempts_ = 0;
                emit connection_state_changed(true, popularity_);
            } else {
                blog(LOG_WARNING, "[danmaku-open] auth failed: %s", pkt.body.toStdString().c_str());
                ws_->close();
            }
        } else if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::HeartbeatReply)) {
            if (pkt.body.size() >= 4) {
                popularity_ = static_cast<int>(
                    qFromBigEndian<uint32_t>(pkt.body.constData()));
                emit connection_state_changed(true, popularity_);
            }
        } else if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::Message)) {
            auto event = danmaku::DanmakuParser::parse(pkt.body.toStdString());
            if (event) {
                switch (event->type) {
                case danmaku::EventType::Danmaku:
                    emit danmaku_received(event->danmaku);
                    break;
                case danmaku::EventType::Gift:
                    emit gift_received(event->gift);
                    break;
                case danmaku::EventType::SuperChat:
                    emit super_chat_received(event->super_chat);
                    break;
                case danmaku::EventType::Entry:
                    emit entry_received(event->entry);
                    break;
                default:
                    break;
                }
            }
        }
    }
}

void DanmakuWebSocket::start_heartbeat()
{
    heartbeat_timer_->start(30000);
}

void DanmakuWebSocket::stop_heartbeat()
{
    heartbeat_timer_->stop();
}

void DanmakuWebSocket::send_heartbeat()
{
    if (!authenticated_) return;

    QByteArray packet = danmaku::DanmakuCodec::encode_packet(
        danmaku::OpCode::Heartbeat,
        danmaku::ProtoVer::Popularity,
        QByteArray(),
        seq_++
    );

    ws_->sendBinaryMessage(packet);
    start_heartbeat();
}

void DanmakuWebSocket::send_open_http_heartbeat()
{
    if (intentional_disconnect_ || open_game_id_.empty() || !cfg_) return;

    std::string gid = open_game_id_;
    std::string ak = cfg_->danmaku.open_live_access_key;
    std::string sk = cfg_->danmaku.open_live_secret;

    std::thread([this, gid, ak, sk]() {
        danmaku::OpenLiveClient::send_heartbeat(gid, ak, sk);
    }).detach();

    open_heartbeat_timer_->start(20000);
}

void DanmakuWebSocket::start_reconnect(int base_delay_ms)
{
    if (intentional_disconnect_) return;

    int delay = base_delay_ms * (1 << std::min(reconnect_attempts_, 4));
    delay = std::min(delay, RECONNECT_MAX_DELAY_MS);
    reconnect_attempts_++;

    blog(LOG_INFO, "[danmaku-open] scheduling reconnect in %d ms (attempt %d)",
         delay, reconnect_attempts_);
    reconnect_timer_->start(delay);
}

void DanmakuWebSocket::stop_reconnect()
{
    reconnect_timer_->stop();
}

void DanmakuWebSocket::attempt_reconnect()
{
    if (intentional_disconnect_) return;
    connect_to_room(room_id_);
}
