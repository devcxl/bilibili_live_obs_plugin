#include "danmaku-ws.h"

#include <obs-module.h>
#include <QtEndian>
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(ws_, &QWebSocket::errorOccurred, this, &DanmakuWebSocket::on_ws_error);
#else
    connect(ws_, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &DanmakuWebSocket::on_ws_error);
#endif
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
    if (room_id.empty()) {
        blog(LOG_ERROR, "[danmaku] connect_to_room called with empty room_id");
        return;
    }

    // disconnect 前先保存 room_id，避免 disconnect_from_room 清空后丢失
    std::string saved_room_id = room_id;

    disconnect_from_room();
    room_id_ = saved_room_id;
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

    host_ = hosts[0].value("host", "");
    if (host_.empty()) {
        blog(LOG_WARNING, "[danmaku] host field missing in host_list");
        start_reconnect();
        return;
    }
    wss_port_ = hosts[0].value("wss_port", 443);

    QString wss_url = QString("wss://%1:%2/sub").arg(
        QString::fromStdString(host_)).arg(wss_port_);
    blog(LOG_INFO, "[danmaku] connecting to %s", wss_url.toStdString().c_str());
    ws_->ignoreSslErrors();  // 预处理忽略，避免 sslErrors 信号触发后才忽略
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
    int64_t room_id_num = 0;
    try {
        room_id_num = std::stoll(room_id_);
    } catch (const std::exception &e) {
        blog(LOG_ERROR, "[danmaku] invalid room_id: %s", room_id_.c_str());
        return;
    }

    json auth_body = {
        {"uid", 0},
        {"roomid", room_id_num},
        {"protover", 3},
        {"platform", "web"},
        {"type", 2},
        {"key", token_},
        {"buvid", ""}   // 必须存在，可空字符串
    };

    std::string body_str = auth_body.dump();
    QByteArray header = build_packet_header(
        static_cast<uint32_t>(16 + body_str.size()),
        1,   // protover = 1 (对齐 champkeh/blive-ws)
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
    qToBigEndian(packet_len, header.data());                   // offset 0: packet_len (4B)
    qToBigEndian<uint16_t>(16, header.data() + 4);             // offset 4: header_len = 16 (2B)
    qToBigEndian<uint16_t>(protover, header.data() + 6);       // offset 6: protover (2B)
    qToBigEndian(op, header.data() + 8);                       // offset 8: op (4B)
    qToBigEndian(seq, header.data() + 12);                     // offset 12: seq (4B)
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
        uint32_t packet_len = qFromBigEndian<uint32_t>(
            buffer.constData() + offset);
        // uint16_t header_len at offset+4, fixed 16
        uint16_t protover = qFromBigEndian<uint16_t>(
            buffer.constData() + offset + 6);
        uint32_t op = qFromBigEndian<uint32_t>(
            buffer.constData() + offset + 8);
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
                    qFromBigEndian<uint32_t>(body.constData()));
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
        if (!j.contains("info") || !j["info"].is_array() || j["info"].size() < 4) {
            blog(LOG_WARNING, "[danmaku] malformed DANMU_MSG: missing info array");
            return;
        }
        auto &info = j["info"];
        if (info.size() > 1 && info[1].is_string()) {
            msg.message = info[1].get<std::string>();
        }
        if (info.size() > 2 && info[2].is_array() && info[2].size() >= 2) {
            if (info[2][1].is_string()) {
                msg.username = info[2][1].get<std::string>();
            }
            if (info[2][0].is_number()) {
                msg.uid = std::to_string(info[2][0].get<int64_t>());
            }
        }
        if (info.size() > 3 && info[3].is_array() && info[3].size() >= 2) {
            if (info[3][1].is_string()) {
                msg.fan_badge = info[3][1].get<std::string>();
            }
            if (info[3][0].is_number()) {
                msg.fan_badge_level = info[3][0].get<int>();
            }
        }

        // 环形缓冲区写入
        message_cache_[cache_write_pos_] = msg;
        cache_write_pos_ = (cache_write_pos_ + 1) % CACHE_SIZE;
        if (cache_count_ < CACHE_SIZE) cache_count_++;

        emit danmaku_received(msg);

    } else if (cmd == "SEND_GIFT") {
        if (!j.contains("data") || !j["data"].is_object()) return;
        auto &d = j["data"];
        GiftMessage gift;
        gift.username = d.value("uname", "");
        gift.gift_name = d.value("giftName", "");
        gift.num = d.value("num", 0);
        gift.combo_num = d.value("combo_num", 0);
        gift.action = d.value("action", "");
        emit gift_received(gift);

    } else if (cmd == "SUPER_CHAT_MESSAGE") {
        if (!j.contains("data") || !j["data"].is_object()) return;
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

    if (reconnect_attempts_ > 31) reconnect_attempts_ = 31;

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
    static const DanmakuMessage empty;
    if (index >= cache_count_) return empty;
    size_t real_index;
    if (cache_count_ < CACHE_SIZE) {
        real_index = index;
    } else {
        real_index = (cache_write_pos_ + index) % CACHE_SIZE;
    }
    return message_cache_[real_index];
}

size_t DanmakuWebSocket::cached_message_count() const
{
    return cache_count_;
}
