#include "danmaku-ws.h"

#include <obs-module.h>
#include <QtEndian>
#include <brotli/decode.h>
#include <zlib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <QMetaObject>

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
    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }
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

    // getDanmuInfo（含 WBI 签名 + buvid3，多次 HTTP）在独立后台线程执行，
    // 避免同步阻塞 UI 线程。完成后通过 Qt 事件队列安全投递回主线程继续连接。
    const uint64_t gen = ++connect_gen_;
    BilibiliApi *api = api_;
    std::string current_room = room_id_;

    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }

    fetch_thread_ = std::thread([this, api, current_room, gen]() {
        ApiResult res = api->get_danmu_info(current_room);
        QMetaObject::invokeMethod(this, [this, gen, res = std::move(res)]() {
            on_danmu_info_ready(gen, res);
        }, Qt::QueuedConnection);
    });
}

void DanmakuWebSocket::on_danmu_info_ready(uint64_t gen, const ApiResult &res)
{
    // 过期结果（期间又 connect/disconnect 过）直接丢弃
    if (gen != connect_gen_) {
        blog(LOG_INFO, "[danmaku] stale getDanmuInfo result dropped");
        return;
    }
    if (!res.ok || res.code != 0 || !res.data.contains("data")
        || !res.data["data"].is_object()) {
        blog(LOG_WARNING, "[danmaku] getDanmuInfo failed: code=%d msg=%s",
             res.code, res.msg.c_str());
        emit connection_state_changed(false, 0);
        start_reconnect();
        return;
    }

    auto &data = res.data["data"];
    token_ = data.value("token", "");
    auto &hosts = data["host_list"];
    if (!hosts.is_array() || hosts.empty()
        || !hosts[0].is_object() || !hosts[0].contains("host")) {
        blog(LOG_WARNING, "[danmaku] host_list empty or malformed");
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
    ws_->open(QUrl(wss_url));
}

void DanmakuWebSocket::disconnect_from_room()
{
    intentional_disconnect_ = true;
    ++connect_gen_;   // 使进行中的异步 getDanmuInfo 结果失效
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
    // 证书校验失败：不忽略，主动断开（触发重连）。
    // 关闭 TLS 校验会使认证包 token 与弹幕内容可被中间人伪造，安全风险不可接受。
    ws_->abort();
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
        {"key", token_}
    };

    blog(LOG_INFO, "[danmaku] sending auth for room=%lld token_len=%zu",
         static_cast<long long>(room_id_num), token_.size());

    std::string body_str = auth_body.dump();
    QByteArray header = build_packet_header(
        static_cast<uint32_t>(16 + body_str.size()),
        1,   // protover = 1
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

void DanmakuWebSocket::parse_packet_loop(const QByteArray &buffer, int depth)
{
    // 限制嵌套深度：合法数据最多两层（WS 消息 → brotli 解压 → 明文包），
    // 构造“解压后仍是 protover=3 压缩包”的嵌套载荷可导致无限递归栈溢出
    if (depth > 2) {
        blog(LOG_WARNING, "[danmaku] packet nesting too deep (%d), dropping", depth);
        return;
    }

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
                emit connection_state_changed(true, popularity_);
            }
        } else if (op == 5) {
            // 业务消息
            if (protover == 0) {
                // 未压缩
                process_message(body.toStdString());
            } else if (protover == 2) {
                // zlib / deflate 压缩
                QByteArray decompressed = zlib_decompress(body);
                if (!decompressed.isEmpty()) {
                    parse_packet_loop(decompressed, depth + 1);
                } else {
                    blog(LOG_WARNING, "[danmaku] zlib decompress failed or oversized, aborting");
                    ws_->abort();
                    return;
                }
            } else if (protover == 3) {
                // Brotli 压缩
                QByteArray decompressed = brotli_decompress(body);
                if (!decompressed.isEmpty()) {
                    parse_packet_loop(decompressed, depth + 1);
                } else {
                    // 解压失败或输出超限：视为异常输入，断开连接（触发重连）
                    blog(LOG_WARNING, "[danmaku] brotli decompress failed or oversized, aborting");
                    ws_->abort();
                    return;
                }
            }
        }

        offset += packet_len;
    }
}

// ── zlib / deflate 解压 ──

QByteArray DanmakuWebSocket::zlib_decompress(const QByteArray &compressed)
{
    if (compressed.isEmpty()) return {};

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));

    if (inflateInit(&strm) != Z_OK) {
        return {};
    }

    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
    strm.avail_in = static_cast<uInt>(compressed.size());

    constexpr size_t CHUNK = 65536;
    constexpr size_t MAX_DECOMPRESSED = 8 * 1024 * 1024;
    std::vector<char> out_buf(CHUNK);
    QByteArray result;

    int ret = Z_OK;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(out_buf.data());
        strm.avail_out = CHUNK;

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&strm);
            return {};
        }

        size_t have = CHUNK - strm.avail_out;
        result.append(out_buf.data(), static_cast<int>(have));

        if (result.size() > static_cast<int>(MAX_DECOMPRESSED)) {
            blog(LOG_WARNING, "[danmaku] zlib output exceeds %zu bytes, abort",
                 MAX_DECOMPRESSED);
            inflateEnd(&strm);
            return {};
        }
    } while (strm.avail_out == 0 && ret != Z_STREAM_END);

    inflateEnd(&strm);
    return result;
}

// ── Brotli 解压 ──

QByteArray DanmakuWebSocket::brotli_decompress(const QByteArray &compressed)
{
    // 单包解压输出硬上限：防止压缩炸弹耗尽内存（弹幕包正常远小于此值）
    constexpr size_t MAX_DECOMPRESSED = 8 * 1024 * 1024;

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
        if (result.size() > static_cast<int>(MAX_DECOMPRESSED)) {
            blog(LOG_WARNING, "[danmaku] brotli output exceeds %zu bytes, abort",
                 MAX_DECOMPRESSED);
            BrotliDecoderDestroyInstance(state);
            return {};
        }
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
            // info[3][10] = guard_level（大航海等级：0=无，1=总督，2=提督，3=舰长）
            if (info[3].size() > 10 && info[3][10].is_number()) {
                msg.guard_level = info[3][10].get<int>();
            }
        }

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

    } else if (cmd == "INTERACT_WORD") {
        if (!j.contains("data") || !j["data"].is_object()) return;
        auto &d = j["data"];
        EntryMessage entry;
        entry.username = d.value("uname", "");
        if (d.contains("uid")) {
            if (d["uid"].is_number()) {
                entry.uid = std::to_string(d["uid"].get<int64_t>());
            } else if (d["uid"].is_string()) {
                entry.uid = d["uid"].get<std::string>();
            }
        }
        entry.msg_type = d.value("msg_type", 1);
        if (d.contains("fans_medal") && d["fans_medal"].is_object()) {
            auto &fm = d["fans_medal"];
            entry.medal_name = fm.value("medal_name", "");
            entry.medal_level = fm.value("medal_level", 0);
            entry.guard_level = fm.value("guard_level", 0);
        }
        if (!entry.username.empty()) {
            emit entry_received(entry);
        }

    } else if (cmd == "ENTRY_EFFECT") {
        if (!j.contains("data") || !j["data"].is_object()) return;
        auto &d = j["data"];
        EntryMessage entry;
        std::string cw = d.value("copy_writing", "");
        if (cw.empty()) {
            cw = d.value("copy_writing_v2", "");
        }
        auto start = cw.find("<%");
        auto end = cw.find("%>");
        if (start != std::string::npos && end != std::string::npos && end > start + 2) {
            entry.username = cw.substr(start + 2, end - start - 2);
        } else {
            entry.username = d.value("uname", "");
        }
        if (d.contains("uid")) {
            if (d["uid"].is_number()) {
                entry.uid = std::to_string(d["uid"].get<int64_t>());
            } else if (d["uid"].is_string()) {
                entry.uid = d["uid"].get<std::string>();
            }
        }
        entry.guard_level = d.value("privilege_type", 3);
        entry.msg_type = 1;
        if (!entry.username.empty()) {
            emit entry_received(entry);
        }
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

    // 指数退避：1s → 2s → 4s → … → 30s 封顶。用乘 2 递推代替 (1 << n)，
    // 避免尝试次数过多时移位/乘法溢出（1<<31 是未定义行为，且 1000*2^25 已超 int 范围），
    // 溢出会导致负值被 QTimer 当作 0ms 立即触发，退化为无间隔重连风暴。
    int delay = RECONNECT_BASE_DELAY_MS;
    for (int n = reconnect_attempts_; n > 0 && delay < RECONNECT_MAX_DELAY_MS; --n) {
        delay *= 2;
    }
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

