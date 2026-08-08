/**
 * @file FrameCodec.cpp
 * @brief 控制帧编解码实现 —— 大端序字节读写
 */
#include "yunyi/protocol/FrameCodec.h"
#include <cstring>

namespace yunyi {
namespace protocol {

namespace {

/**
 * @brief 写入 16 位大端整数到缓冲区
 */
inline void writeU16(uint8_t* buf, uint16_t val) {
    buf[0] = static_cast<uint8_t>(val >> 8);
    buf[1] = static_cast<uint8_t>(val & 0xFF);
}

/**
 * @brief 写入 32 位大端整数到缓冲区
 */
inline void writeU32(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>(val >> 24);
    buf[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(val & 0xFF);
}

/**
 * @brief 从缓冲区读取 16 位大端整数
 */
inline uint16_t readU16(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

/**
 * @brief 从缓冲区读取 32 位大端整数
 */
inline uint32_t readU32(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24)
         | (static_cast<uint32_t>(buf[1]) << 16)
         | (static_cast<uint32_t>(buf[2]) << 8)
         | buf[3];
}

/**
 * @brief 写入帧头部: magic + version + type + payload_len
 * @param[in,out] out 输出 vector
 * @param type 帧类型
 * @param payloadLen 负载长度（字节）
 */
void writeHeader(std::vector<uint8_t>& out, FrameType type, uint32_t payloadLen) {
    out.push_back(kMagic);
    out.push_back(kVersion);
    out.push_back(static_cast<uint8_t>(type));
    uint8_t lenBuf[4];
    writeU32(lenBuf, payloadLen);
    out.insert(out.end(), lenBuf, lenBuf + 4);
}

} // anonymous namespace

// ============================================================
//  编码
// ============================================================

std::vector<uint8_t> FrameCodec::encodeRegister(const RegisterPayload& p) {
    // 负载: roomName(N bytes) + localMcPort(2 bytes) + roomId(4 bytes, 大端)
    uint32_t payloadLen = static_cast<uint32_t>(p.roomName.size()) + 2 + 4;
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::REGISTER, payloadLen);

    out.insert(out.end(),
        reinterpret_cast<const uint8_t*>(p.roomName.data()),
        reinterpret_cast<const uint8_t*>(p.roomName.data()) + p.roomName.size());

    uint8_t buf[6];
    writeU16(buf, p.localMcPort);
    writeU32(buf + 2, p.roomId);
    out.insert(out.end(), buf, buf + 6);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeRegisterAck(const RegisterAckPayload& p) {
    // 负载: assignedPort(2) + roomId(4) = 6 bytes
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::REGISTER_ACK, 6);
    uint8_t buf[6];
    writeU16(buf, p.assignedPort);
    writeU32(buf + 2, p.roomId);
    out.insert(out.end(), buf, buf + 6);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeHeartbeat() {
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::HEARTBEAT, 0);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeHeartbeatAck() {
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::HEARTBEAT_ACK, 0);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeOpenStream(const OpenStreamPayload& p) {
    // 负载: playerConnId(4) + tunnelPort(2) + flags(1) = 7 bytes
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::OPEN_STREAM, 7);
    uint8_t buf[7];
    writeU32(buf, p.playerConnId);
    writeU16(buf + 4, p.tunnelPort);
    buf[6] = p.flags;
    out.insert(out.end(), buf, buf + 7);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeStreamBind(const StreamBindPayload& p) {
    // 负载: playerConnId(4) = 4 bytes
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::STREAM_BIND, 4);
    uint8_t buf[4];
    writeU32(buf, p.playerConnId);
    out.insert(out.end(), buf, buf + 4);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeDeregister() {
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::DEREGISTER, 0);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeHolePunch(const HolePunchPayload& p) {
    // 负载: candidateIp(N) + candidatePort(2) + playerConnId(4)
    uint32_t payloadLen = static_cast<uint32_t>(p.candidateIp.size()) + 2 + 4;
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::HOLE_PUNCH, payloadLen);
    out.insert(out.end(),
        reinterpret_cast<const uint8_t*>(p.candidateIp.data()),
        reinterpret_cast<const uint8_t*>(p.candidateIp.data()) + p.candidateIp.size());
    uint8_t buf[6];
    writeU16(buf, p.candidatePort);
    writeU32(buf + 2, p.playerConnId);
    out.insert(out.end(), buf, buf + 6);
    return out;
}

std::vector<uint8_t> FrameCodec::encodeError(ErrorCode code, std::string_view message) {
    // 负载: errorCode(1) + message(N bytes)
    uint32_t payloadLen = 1 + static_cast<uint32_t>(message.size());
    std::vector<uint8_t> out;
    writeHeader(out, FrameType::ERROR, payloadLen);
    out.push_back(static_cast<uint8_t>(code));
    out.insert(out.end(),
        reinterpret_cast<const uint8_t*>(message.data()),
        reinterpret_cast<const uint8_t*>(message.data()) + message.size());
    return out;
}

// ============================================================
//  解码
// ============================================================

bool FrameCodec::decodeHeader(const uint8_t* header,
                               FrameType& outType,
                               uint8_t& outVersion,
                               uint32_t& outPayloadLen) {
    if (!header || header[0] != kMagic) return false;
    outVersion    = header[1];
    outType       = static_cast<FrameType>(header[2]);
    outPayloadLen = readU32(header + 3);
    return true;
}

RegisterPayload FrameCodec::decodeRegisterPayload(const uint8_t* data, uint32_t len) {
    RegisterPayload p;
    if (len < 6) return p;  // 最少: 2(port) + 4(roomId)
    // 前 len-6 字节为房间名，然后是 2 字节端口 + 4 字节 roomId
    uint32_t nameLen = (len > 6) ? (len - 6) : 0;
    if (nameLen > 0) {
        p.roomName.assign(reinterpret_cast<const char*>(data), nameLen);
    }
    p.localMcPort = readU16(data + nameLen);
    p.roomId      = readU32(data + nameLen + 2);
    return p;
}

RegisterAckPayload FrameCodec::decodeRegisterAckPayload(const uint8_t* data, uint32_t len) {
    RegisterAckPayload p;
    if (len < 6) return p;
    p.assignedPort = readU16(data);
    p.roomId       = readU32(data + 2);
    return p;
}

OpenStreamPayload FrameCodec::decodeOpenStreamPayload(const uint8_t* data, uint32_t len) {
    OpenStreamPayload p;
    if (len < 7) return p;
    p.playerConnId = readU32(data);
    p.tunnelPort   = readU16(data + 4);
    p.flags        = data[6];
    return p;
}

StreamBindPayload FrameCodec::decodeStreamBindPayload(const uint8_t* data, uint32_t len) {
    StreamBindPayload p;
    if (len < 4) return p;
    p.playerConnId = readU32(data);
    return p;
}

HolePunchPayload FrameCodec::decodeHolePunchPayload(const uint8_t* data, uint32_t len) {
    HolePunchPayload p;
    if (len < 6) return p;  // 最少: 2(port) + 4(playerConnId)
    // 前 len-6 字节为候选 IP，然后是 2 字节端口 + 4 字节 playerConnId
    p.candidateIp.assign(reinterpret_cast<const char*>(data), len - 6);
    p.candidatePort = readU16(data + len - 6);
    p.playerConnId  = readU32(data + len - 2);
    return p;
}

ErrorPayload FrameCodec::decodeErrorPayload(const uint8_t* data, uint32_t len) {
    ErrorPayload p;
    if (len < 1) return p;
    p.code = static_cast<ErrorCode>(data[0]);
    if (len > 1) {
        p.message.assign(reinterpret_cast<const char*>(data + 1), len - 1);
    }
    return p;
}

} // namespace protocol
} // namespace yunyi
