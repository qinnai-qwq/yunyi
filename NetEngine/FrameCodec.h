/**
 * @file FrameCodec.h
 * @brief 控制帧编解码 —— 帧格式常量、帧类型枚举、错误码枚举
 *
 * 与 docs/protocol.md 同步，所有魔数和编码规则以此处为唯一权威来源。
 *
 * 帧格式（大端序）:
 *   magic(1) | version(1) | type(1) | payload_len(4) | payload(N)
 *   共 7 字节头部 + N 字节负载。
 */
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
// Windows 头文件将 ERROR 定义为 0，会破坏 FrameType::ERROR，需先解除
#ifdef ERROR
#undef ERROR
#endif
namespace yunyi {
namespace protocol {

/** 帧头部固定 magic 值 0xC5 */
constexpr uint8_t kMagic = 0xC5;

/** 当前协议版本号 */
constexpr uint8_t kVersion = 0x01;

/** 帧头部长度: magic(1) + version(1) + type(1) + payload_len(4) = 7 */
constexpr size_t kHeaderSize = 7;

/**
 * @brief 帧类型枚举
 *
 * 编码空间:
 *   0x01-0x0F  核心帧类型
 *   0x10-0x1F  预留扩展
 *   0xF0-0xFE  调试用
 *   0xFF       ERROR 帧
 */
enum class FrameType : uint8_t {
    /** 房主注册房间 */
    REGISTER = 0x01,
    /** 注册应答 */
    REGISTER_ACK = 0x02,
    /** 心跳请求 */
    HEARTBEAT = 0x03,
    /** 心跳应答 */
    HEARTBEAT_ACK = 0x04,
    /** 请求打开数据隧道 */
    OPEN_STREAM = 0x05,
    /** 隧道绑定通知 */
    STREAM_BIND = 0x06,
    /** 注销房间 */
    DEREGISTER = 0x07,
    /** NAT 打洞候选端点互换（中继协调直连） */
    HOLE_PUNCH = 0x10,
    /** 错误通知 */
    ERROR = 0xFF
};

/**
 * @brief 判断是否为核心帧类型 (0x01-0x0F)
 * @param t 帧类型字节
 * @return true 是核心帧
 */
inline bool isCoreFrame(uint8_t t) { return t >= 0x01 && t <= 0x0F; }

/**
 * @brief 判断是否为预留帧类型 (0x10-0x1F)
 * @param t 帧类型字节
 * @return true 是预留帧
 */
inline bool isReserved(uint8_t t) { return t >= 0x10 && t <= 0x1F; }

/**
 * @brief 错误码枚举
 */
enum class ErrorCode : uint8_t {
    /** 协议版本不匹配 */
    PROTOCOL_VERSION_MISMATCH = 0x01,
    /** 未知帧类型 */
    UNKNOWN_FRAME_TYPE = 0x02,
    /** 房间不存在 */
    ROOM_NOT_FOUND = 0x03,
    /** 端口池耗尽 */
    PORT_POOL_EXHAUSTED = 0x04,
    /** 隧道建立失败 */
    TUNNEL_SETUP_FAILED = 0x05,
    /** 认证失败 */
    AUTH_FAILED = 0x06,
    /** 超时 */
    TIMEOUT = 0x07,
    /** 无效负载 */
    INVALID_PAYLOAD = 0x08,
    /** 房间已满 */
    ROOM_FULL = 0x09,
    /** 内部错误 */
    INTERNAL = 0x0A,
    /** 已注册（重复注册） */
    ALREADY_REGISTERED = 0x0B,
    /** 流绑定失败 */
    STREAM_BIND_FAILED = 0x0C,
};

/**
 * @brief 将错误码转为人类可读字符串
 * @param c 错误码
 * @return 英文错误描述
 */
inline const char* errorCodeToString(ErrorCode c) {
    switch (c) {
    case ErrorCode::PROTOCOL_VERSION_MISMATCH: return "protocol version mismatch";
    case ErrorCode::UNKNOWN_FRAME_TYPE:       return "unknown frame type";
    case ErrorCode::ROOM_NOT_FOUND:           return "room not found";
    case ErrorCode::PORT_POOL_EXHAUSTED:      return "port pool exhausted";
    case ErrorCode::TUNNEL_SETUP_FAILED:      return "tunnel setup failed";
    case ErrorCode::AUTH_FAILED:              return "authentication failed";
    case ErrorCode::TIMEOUT:                  return "timeout";
    case ErrorCode::INVALID_PAYLOAD:          return "invalid payload";
    case ErrorCode::ROOM_FULL:                return "room full";
    case ErrorCode::INTERNAL:                 return "internal error";
    case ErrorCode::ALREADY_REGISTERED:       return "already registered";
    case ErrorCode::STREAM_BIND_FAILED:       return "stream bind failed";
    }
    return "unknown error";
}

/**
 * @brief ERROR 帧负载
 */
struct ErrorPayload {
    /** 错误码 */
    ErrorCode code = ErrorCode::INTERNAL;
    /** 错误描述信息 */
    std::string message;
};

/**
 * @brief REGISTER 帧负载（房主 -> 中继）
 */
struct RegisterPayload {
    /** 房间名称，1-64 字符 UTF-8 */
    std::string roomName;
    /** 本机 MC 服务端口 */
    uint16_t localMcPort = 0;
    /** 房间 ID（0 = 创建新房间，非 0 = 认领已有房间） */
    uint32_t roomId = 0;
};

/**
 * @brief REGISTER_ACK 帧负载（中继 -> 房主）
 */
struct RegisterAckPayload {
    /** 分配的公网端口 */
    uint16_t assignedPort = 0;
    /** 分配的房间 ID */
    uint32_t roomId = 0;
};

/**
 * @brief OPEN_STREAM 帧负载（中继 -> 房主）
 */
struct OpenStreamPayload {
    /** 玩家连接 ID */
    uint32_t playerConnId = 0;
    /** 隧道端口（房主 TLS 数据隧道连接此端口） */
    uint16_t tunnelPort = 0;
    /** 标志位（预留） */
    uint8_t flags = 0;
};

/**
 * @brief STREAM_BIND 帧负载（房主 -> 中继）
 */
struct StreamBindPayload {
    /** 绑定的玩家连接 ID */
    uint32_t playerConnId = 0;
};

/**
 * @brief HOLE_PUNCH 帧负载（NAT 打洞候选端点互换）
 *
 * 中继协调模式：中继把房主候选端点发给玩家、把玩家候选端点发给房主，
 * 双方据此同时向对方公网映射端点发 UDP 打洞包建立直连。
 * 负载: candidateIp(N) + candidatePort(2) + playerConnId(4)
 */
struct HolePunchPayload {
    /** 候选公网映射 IP（STUN 观察到的源地址） */
    std::string candidateIp;
    /** 候选公网映射端口 */
    uint16_t candidatePort = 0;
    /** 关联的玩家连接 ID（房主侧匹配数据隧道用，无则 0） */
    uint32_t playerConnId = 0;
};

/**
 * @class FrameCodec
 * @brief 控制帧编解码器（纯静态方法）
 *
 * 所有 encode 方法返回完整的帧字节（含头部 + 负载）。
 * 所有 decode 方法从负载数据中解析结构体。
 *
 * @note 所有多字节整数使用大端序（网络字节序）。
 */
class FrameCodec {
public:
    // ======================== 编码 ========================

    /**
     * @brief 编码 REGISTER 帧
     * @param p 注册负载
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeRegister(const RegisterPayload& p);

    /**
     * @brief 编码 REGISTER_ACK 帧
     * @param p 应答负载
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeRegisterAck(const RegisterAckPayload& p);

    /**
     * @brief 编码 HEARTBEAT 帧（无负载）
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeHeartbeat();

    /**
     * @brief 编码 HEARTBEAT_ACK 帧（无负载）
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeHeartbeatAck();

    /**
     * @brief 编码 OPEN_STREAM 帧
     * @param p 开流负载
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeOpenStream(const OpenStreamPayload& p);

    /**
     * @brief 编码 STREAM_BIND 帧
     * @param p 流绑定负载
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeStreamBind(const StreamBindPayload& p);

    /**
     * @brief 编码 DEREGISTER 帧（无负载）
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeDeregister();

    /**
     * @brief 编码 HOLE_PUNCH 帧（NAT 打洞候选端点互换）
     * @param p 打洞负载
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeHolePunch(const HolePunchPayload& p);

    /**
     * @brief 编码 ERROR 帧
     * @param code 错误码
     * @param msg 错误描述
     * @return 完整帧字节
     */
    static std::vector<uint8_t> encodeError(ErrorCode code, std::string_view msg);

    // ======================== 解码 ========================

    /**
     * @brief 解码帧头部
     * @param h 指向至少 7 字节帧头部的指针
     * @param[out] t 帧类型
     * @param[out] v 协议版本
     * @param[out] pl 负载长度
     * @return true 头部合法（magic 匹配），false 非法
     * @pre h 指向至少 kHeaderSize 字节的有效内存
     */
    static bool decodeHeader(const uint8_t* h, FrameType& t, uint8_t& v, uint32_t& pl);

    /**
     * @brief 解码 REGISTER 帧负载
     * @param d 负载数据指针
     * @param len 负载长度
     * @return RegisterPayload
     */
    static RegisterPayload decodeRegisterPayload(const uint8_t* d, uint32_t len);

    /**
     * @brief 解码 REGISTER_ACK 帧负载
     * @param d 负载数据指针
     * @param len 负载长度
     * @return RegisterAckPayload
     */
    static RegisterAckPayload decodeRegisterAckPayload(const uint8_t* d, uint32_t len);

    /**
     * @brief 解码 OPEN_STREAM 帧负载
     * @param d 负载数据指针
     * @param len 负载长度
     * @return OpenStreamPayload
     */
    static OpenStreamPayload decodeOpenStreamPayload(const uint8_t* d, uint32_t len);

    /**
     * @brief 解码 STREAM_BIND 帧负载
     * @param d 负载数据指针
     * @param len 负载长度
     * @return StreamBindPayload
     */
    static StreamBindPayload decodeStreamBindPayload(const uint8_t* d, uint32_t len);

    /**
     * @brief 解码 HOLE_PUNCH 帧负载
     * @param d 负载数据指针
     * @param len 负载长度
     * @return HolePunchPayload
     */
    static HolePunchPayload decodeHolePunchPayload(const uint8_t* d, uint32_t len);

    /**
     * @brief 解码 ERROR 帧负载
     * @param d 负载数据指针
     * @param len 负载长度
     * @return ErrorPayload
     */
    static ErrorPayload decodeErrorPayload(const uint8_t* d, uint32_t len);
};

}
}
