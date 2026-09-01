#pragma once

#include <vector>
#include <QByteArray>
#include "danmaku-packet.h"

namespace danmaku {

class DanmakuCodec {
public:
    // 将 OpCode 和 Body 封装为符合官方规范的 16 字节大端协议包
    static QByteArray encode_packet(OpCode op, ProtoVer protover,
                                    const QByteArray &body = QByteArray(),
                                    uint32_t seq = 1);

    // 将接收到的原始二进制 Buffer 解析为一个或多个协议包（处理粘包、递归解压缩与错误防护）
    static std::vector<RawPacket> decode_packets(const QByteArray &buffer, int depth = 0);

    // 安全解压 zlib (deflate)
    static QByteArray decompress_zlib(const QByteArray &compressed);

    // 安全解压 brotli
    static QByteArray decompress_brotli(const QByteArray &compressed);

    // 安全上限
    static constexpr size_t MAX_DECOMPRESSED_SIZE = 8 * 1024 * 1024; // 8MB
    static constexpr int MAX_RECURSION_DEPTH = 2;
};

} // namespace danmaku
