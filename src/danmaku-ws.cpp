#include "danmaku-ws.h"

#include <obs-module.h>
#include <arpa/inet.h>
#include <brotli/decode.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── 构造 / 析构 ──

DanmakuWebSocket::DanmakuWebSocket(QObject *parent)
    : QObject(parent)
{
    ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    heartbeat_timer_ = new QTimer(this);
    heartbeat_timer_->setSingleShot(true);
    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);

    message_cache_.resize(CACHE_SIZE);

    connect(ws_, &QWebSocket::connected, this, &DanmakuWebSocket::on_ws_connected);
    connect(ws_, &QWebSocket::disconnected, this, &DanmakuWebSocket::on_ws_disconnected);
    connect(ws_, &QWebSocket::binaryMessageReceived,
            this, &DanmakuWebSocket::on_ws_binary_message);
    connect(ws_, &QWebSocket::errorOccurred, this, &DanmakuWebSocket::on_ws_error);
    connect(ws_, &QWebSocket::sslErrors, this, &DanmakuWebSocket::on_ws_ssl_errors);
    connect(heartbeat_timer_, &QTimer::timeout, this, &DanmakuWebSocket::send_heartbeat);
    connect(reconnect_timer_, &QTimer::timeout, this, &DanmakuWebSocket::attempt_reconnect);
}

DanmakuWebSocket::~DanmakuWebSocket()
{
    disconnect_from_room();
}

// ── 依赖注入 / 查询 ──

void DanmakuWebSocket::set_api(BilibiliApi *api) { api_ = api; }

bool DanmakuWebSocket::is_connected() const { return authenticated_; }

int DanmakuWebSocket::popularity() const { return popularity_; }

// ── 生命周期 ──

void DanmakuWebSocket::connect_to_room(const std::string &room_id)
{
    if (!api_) {
        blog(LOG_ERROR, "[danmaku] api not set");
        return;
    }

    disconnect_from_room();
    room_id_ = room_id;
    intentional_disconnect_ = false;
    reconnect_attempts_ = 0;

    ApiResult res = api_->get_danmu_info(room_id);
    if (!res.ok || res.code != 0 || !res.data.contains("data")) {
        blog(LOG_WARNING, "[danmaku] getDanmuInfo failed: code=%d msg=%s",
             res.code, res.msg.c_str());
        start_reconnect();
        return;
    }

    auto &data = res.data["data"];
    token_ = data.value("token", "");
    auto &hosts = data["host_list"];
    if (hosts.empty() || !hosts[0].contains("host")) {
        blog(LOG_WARNING, "[danmaku] host_list empty");
        start_reconnect();
        return;
    }

    host_ = hosts[0]["host"].get<std::string>();
    wss_port_ = hosts[0].value("wss_port", 443);

    QString wss_url = QString("wss://%1:%2/sub").arg(
        QString::fromStdString(host_)).arg(wss_port_);
    blog(LOG_INFO, "[danmaku] connecting to %s", wss_url.toStdString().c_str());
    ws_->open(QUrl(wss_url));
}

void DanmakuWebSocket::disconnect_from_room()
{
    intentional_disconnect_ = true;
    stop_heartbeat();
    stop_reconnect();
    authenticated_ = false;
    ws_->close();
    room_id_.clear();
}

// ── WebSocket 事件处理 ──

void DanmakuWebSocket::on_ws_connected()
{
    blog(LOG_INFO, "[danmaku] ws connected, sending auth");
    seq_ = 1;
    send_auth_packet();
}

void DanmakuWebSocket::on_ws_disconnected()
{
    blog(LOG_INFO, "[danmaku] ws disconnected");
    authenticated_ = false;
    stop_heartbeat();
    emit connection_state_changed(false, 0);

    if (!intentional_disconnect_) {
        start_reconnect();
    }
}

void DanmakuWebSocket::on_ws_error(QAbstractSocket::SocketError error)
{
    blog(LOG_WARNING, "[danmaku] ws error: %d", static_cast<int>(error));
}

void DanmakuWebSocket::on_ws_ssl_errors(const QList<QSslError> &errors)
{
    for (const auto &e : errors) {
        blog(LOG_WARNING, "[danmaku] SSL error: %s",
             e.errorString().toStdString().c_str());
    }
    // 不中断连接（可信网络环境）
    ws_->ignoreSslErrors();
}

// ── 认证 ──

void DanmakuWebSocket::send_auth_packet()
{
    json auth_body = {
        {"uid", 0},
        {"roomid", std::stoll(room_id_)},
        {"protover", 3},
        {"platform", "web"},
        {"type", 2},
        {"key", token_}
    };

    std::string body_str = auth_body.dump();
    QByteArray header = build_packet_header(
        static_cast<uint32_t>(16 + body_str.size()),
        1,   // protover
        7,   // op = AUTH
        seq_++
    );

    QByteArray packet = header + QByteArray::fromStdString(body_str);
    ws_->sendBinaryMessage(packet);
}

// ── 包头构建 ──

QByteArray DanmakuWebSocket::build_packet_header(
    uint32_t packet_len, uint16_t protover, uint32_t op, uint32_t seq)
{
    QByteArray header(16, 0);
    *reinterpret_cast<uint32_t*>(header.data()) = htonl(packet_len);
    *reinterpret_cast<uint16_t*>(header.data() + 4) = htons(protover);
    *reinterpret_cast<uint32_t*>(header.data() + 8) = htonl(op);
    *reinterpret_cast<uint32_t*>(header.data() + 12) = htonl(seq);
    return header;
}

// ── 二进制消息接收 / 包解析 ──

void DanmakuWebSocket::on_ws_binary_message(const QByteArray &data)
{
    parse_packet_loop(data);
}

void DanmakuWebSocket::parse_packet_loop(const QByteArray &buffer)
{
    int offset = 0;
    while (offset + 16 <= buffer.size()) {
        uint32_t packet_len = ntohl(*reinterpret_cast<const uint32_t*>(
            buffer.constData() + offset));
        uint16_t protover   = ntohs(*reinterpret_cast<const uint16_t*>(
            buffer.constData() + offset + 4));
        uint32_t op         = ntohl(*reinterpret_cast<const uint32_t*>(
            buffer.constData() + offset + 8));
        // uint32_t seq field at offset+12, ignored

        if (packet_len < 16 || offset + packet_len > static_cast<uint32_t>(buffer.size())) {
            break;
        }

        QByteArray body = buffer.mid(offset + 16, packet_len - 16);

        if (op == 8) {
            // 认证结果
            auto j = json::parse(body.toStdString(), nullptr, false);
            if (!j.is_discarded() && j.value("code", -1) == 0) {
                authenticated_ = true;
                blog(LOG_INFO, "[danmaku] auth success");
                start_heartbeat();
                stop_reconnect();
                reconnect_attempts_ = 0;
                emit connection_state_changed(true, popularity_);
            } else {
                blog(LOG_WARNING, "[danmaku] auth failed: %s",
                     body.toStdString().c_str());
                ws_->close();
            }
        } else if (op == 3) {
            // 心跳应答（人气值）
            if (body.size() >= 4) {
                popularity_ = static_cast<int>(
                    ntohl(*reinterpret_cast<const uint32_t*>(body.constData())));
            }
        } else if (op == 5) {
            // 业务消息
            if (protover == 0) {
                // 未压缩
                process_message(body.toStdString());
            } else if (protover == 3) {
                // Brotli 压缩
                QByteArray decompressed = brotli_decompress(body);
                if (!decompressed.isEmpty()) {
                    parse_packet_loop(decompressed);
                } else {
                    blog(LOG_WARNING, "[danmaku] brotli decompress failed");
                }
            }
        }

        offset += packet_len;
    }
}

// ── Brotli 解压 ──

QByteArray DanmakuWebSocket::brotli_decompress(const QByteArray &compressed)
{
    BrotliDecoderState *state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) return {};

    std::vector<uint8_t> out_buffer(65536);
    size_t available_in = compressed.size();
    const uint8_t *next_in = reinterpret_cast<const uint8_t*>(compressed.constData());

    QByteArray result;
    BrotliDecoderResult rc;
    do {
        size_t available_out = out_buffer.size();
        uint8_t *next_out = out_buffer.data();
        rc = BrotliDecoderDecompressStream(state, &available_in, &next_in,
                                            &available_out, &next_out, nullptr);
        result.append(reinterpret_cast<const char*>(out_buffer.data()),
                      out_buffer.size() - available_out);
    } while (rc == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);

    BrotliDecoderDestroyInstance(state);

    if (rc == BROTLI_DECODER_RESULT_SUCCESS) {
        return result;
    }
    return {};
}

// ── 消息类型分发 ──

void DanmakuWebSocket::process_message(const std::string &json_str)
{
    auto j = json::parse(json_str, nullptr, false);
    if (j.is_discarded()) return;

    std::string cmd = j.value("cmd", "");

    if (cmd == "DANMU_MSG") {
        DanmakuMessage msg;
        msg.cmd = cmd;
        auto &info = j["info"];
        msg.message = info[1].get<std::string>();
        msg.username = info[2][1].get<std::string>();
        msg.uid = std::to_string(info[2][0].get<int64_t>());
        if (!info[3].is_null() && info[3].is_array() && info[3].size() >= 2) {
            msg.fan_badge = info[3][1].get<std::string>();
            msg.fan_badge_level = info[3][0].get<int>();
        }

        // 环形缓冲区写入
        message_cache_[cache_write_pos_] = msg;
        cache_write_pos_ = (cache_write_pos_ + 1) % CACHE_SIZE;
        if (cache_count_ < CACHE_SIZE) cache_count_++;

        emit danmaku_received(msg);

    } else if (cmd == "SEND_GIFT") {
        auto &d = j["data"];
        GiftMessage gift;
        gift.username = d.value("uname", "");
        gift.gift_name = d.value("giftName", "");
        gift.num = d.value("num", 0);
        gift.combo_num = d.value("combo_num", 0);
        gift.action = d.value("action", "");
        emit gift_received(gift);

    } else if (cmd == "SUPER_CHAT_MESSAGE") {
        auto &d = j["data"];
        SuperChatMessage sc;
        sc.username = d.value("uname", "");
        sc.message = d.value("message", "");
        sc.price = d.value("price", 0);
        emit super_chat_received(sc);
    }
    // 未知 cmd 静默忽略
}

// ── 心跳 ──

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

    QByteArray header = build_packet_header(
        16,     // packet_len = header only, no body
        1,      // protover
        2,      // op = HEARTBEAT
        seq_++
    );
    ws_->sendBinaryMessage(header);
    heartbeat_timer_->start(30000);
}

// ── 重连 ──

void DanmakuWebSocket::start_reconnect()
{
    if (intentional_disconnect_ || room_id_.empty()) return;

    int delay = RECONNECT_BASE_DELAY_MS * (1 << reconnect_attempts_);
    if (delay > RECONNECT_MAX_DELAY_MS) delay = RECONNECT_MAX_DELAY_MS;
    reconnect_attempts_++;

    blog(LOG_INFO, "[danmaku] reconnect attempt %d in %dms",
         reconnect_attempts_, delay);
    reconnect_timer_->start(delay);
}

void DanmakuWebSocket::stop_reconnect()
{
    reconnect_timer_->stop();
}

void DanmakuWebSocket::attempt_reconnect()
{
    if (!room_id_.empty()) {
        connect_to_room(room_id_);
    }
}

// ── 消息缓存查询 ──

const DanmakuMessage &DanmakuWebSocket::cached_message(size_t index) const
{
    return message_cache_[index % CACHE_SIZE];
}

size_t DanmakuWebSocket::cached_message_count() const
{
    return cache_count_;
}
