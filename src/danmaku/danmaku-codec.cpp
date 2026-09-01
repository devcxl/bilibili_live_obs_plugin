#include "danmaku-codec.h"
#include <QtEndian>
#include <zlib.h>
#include <brotli/decode.h>
#include <obs-module.h>
#include <cstring>

namespace danmaku {

QByteArray DanmakuCodec::encode_packet(OpCode op, ProtoVer protover,
                                       const QByteArray &body, uint32_t seq)
{
    uint32_t packet_len = HEADER_LENGTH + static_cast<uint32_t>(body.size());
    uint16_t header_len = HEADER_LENGTH;
    uint16_t proto_val = static_cast<uint16_t>(protover);
    uint32_t op_val = static_cast<uint32_t>(op);

    QByteArray packet;
    packet.resize(packet_len);

    qToBigEndian(packet_len,  reinterpret_cast<uchar*>(packet.data()));
    qToBigEndian(header_len,  reinterpret_cast<uchar*>(packet.data() + 4));
    qToBigEndian(proto_val,   reinterpret_cast<uchar*>(packet.data() + 6));
    qToBigEndian(op_val,      reinterpret_cast<uchar*>(packet.data() + 8));
    qToBigEndian(seq,         reinterpret_cast<uchar*>(packet.data() + 12));

    if (!body.isEmpty()) {
        std::memcpy(packet.data() + HEADER_LENGTH, body.constData(), body.size());
    }

    return packet;
}

std::vector<RawPacket> DanmakuCodec::decode_packets(const QByteArray &buffer, int depth)
{
    std::vector<RawPacket> packets;

    if (depth > MAX_RECURSION_DEPTH) {
        blog(LOG_WARNING, "[danmaku-codec] 嵌套解包层级过深 (%d)，丢弃异常数据包", depth);
        return packets;
    }

    int offset = 0;
    while (offset + HEADER_LENGTH <= buffer.size()) {
        uint32_t packet_len = qFromBigEndian<uint32_t>(buffer.constData() + offset);
        uint16_t header_len = qFromBigEndian<uint16_t>(buffer.constData() + offset + 4);
        uint16_t protover = qFromBigEndian<uint16_t>(buffer.constData() + offset + 6);
        uint32_t op = qFromBigEndian<uint32_t>(buffer.constData() + offset + 8);
        uint32_t seq = qFromBigEndian<uint32_t>(buffer.constData() + offset + 12);

        // 基本合法性校验
        if (packet_len < HEADER_LENGTH || offset + packet_len > static_cast<uint32_t>(buffer.size())) {
            break;
        }

        QByteArray body = buffer.mid(offset + HEADER_LENGTH, packet_len - HEADER_LENGTH);

        if (op == static_cast<uint32_t>(OpCode::Message)) {
            if (protover == static_cast<uint16_t>(ProtoVer::Zlib)) {
                QByteArray decomp = decompress_zlib(body);
                if (!decomp.isEmpty()) {
                    auto sub_packets = decode_packets(decomp, depth + 1);
                    packets.insert(packets.end(), sub_packets.begin(), sub_packets.end());
                }
            } else if (protover == static_cast<uint16_t>(ProtoVer::Brotli)) {
                QByteArray decomp = decompress_brotli(body);
                if (!decomp.isEmpty()) {
                    auto sub_packets = decode_packets(decomp, depth + 1);
                    packets.insert(packets.end(), sub_packets.begin(), sub_packets.end());
                }
            } else {
                RawPacket pkt;
                pkt.packet_length = packet_len;
                pkt.header_length = header_len;
                pkt.protover = protover;
                pkt.op = op;
                pkt.seq = seq;
                pkt.body = body;
                packets.push_back(pkt);
            }
        } else {
            RawPacket pkt;
            pkt.packet_length = packet_len;
            pkt.header_length = header_len;
            pkt.protover = protover;
            pkt.op = op;
            pkt.seq = seq;
            pkt.body = body;
            packets.push_back(pkt);
        }

        offset += packet_len;
    }

    return packets;
}

QByteArray DanmakuCodec::decompress_zlib(const QByteArray &compressed)
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

        if (result.size() > static_cast<int>(MAX_DECOMPRESSED_SIZE)) {
            blog(LOG_WARNING, "[danmaku-codec] zlib 解压数据超出 %zu 字节安全上限",
                 MAX_DECOMPRESSED_SIZE);
            inflateEnd(&strm);
            return {};
        }
    } while (strm.avail_out == 0 && ret != Z_STREAM_END);

    inflateEnd(&strm);
    return result;
}

QByteArray DanmakuCodec::decompress_brotli(const QByteArray &compressed)
{
    if (compressed.isEmpty()) return {};

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
        if (result.size() > static_cast<int>(MAX_DECOMPRESSED_SIZE)) {
            blog(LOG_WARNING, "[danmaku-codec] brotli 解压数据超出 %zu 字节安全上限",
                 MAX_DECOMPRESSED_SIZE);
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

} // namespace danmaku
