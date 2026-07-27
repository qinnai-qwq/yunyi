/**
 * @file ConnectionCode.cpp
 * @brief 连接码生成/解析实现
 */
#include "ConnectionCode.h"
#include <algorithm>
#include <cctype>

namespace yunyi {

std::string ConnectionCode::generate(
    const std::string& publicIp, uint16_t port) {
    // IPv6 地址用 [ip]:port 格式（RFC 2732）
    if (publicIp.find(':') != std::string::npos) {
        return "[" + publicIp + "]:" + std::to_string(port);
    }
    return publicIp + ":" + std::to_string(port);
}

bool ConnectionCode::parse(
    std::string_view code, std::string& outIp, uint16_t& outPort) {
    if (code.empty()) return false;

    if (code[0] == '[') {
        // IPv6 格式: [ip]:port
        auto closeBracket = code.find(']');
        if (closeBracket == std::string_view::npos || closeBracket <= 1)
            return false;
        outIp = std::string(code.substr(1, closeBracket - 1));
        // 跳过 ']' 后必须是 ':'
        if (closeBracket + 1 >= code.size() || code[closeBracket + 1] != ':')
            return false;
        std::string portStr(code.substr(closeBracket + 2));
        try {
            int port = std::stoi(portStr);
            if (port <= 0 || port > 65535) return false;
            outPort = static_cast<uint16_t>(port);
        } catch (...) { return false; }
        return true;
    } else {
        // IPv4 格式: ip:port（找最后一个 ':'，允许 IPv4 本身不含 ':'）
        auto colonPos = code.rfind(':');
        if (colonPos == std::string_view::npos
            || colonPos == 0
            || colonPos == code.size() - 1)
            return false;
        outIp = std::string(code.substr(0, colonPos));
        std::string portStr(code.substr(colonPos + 1));
        try {
            int port = std::stoi(portStr);
            if (port <= 0 || port > 65535) return false;
            outPort = static_cast<uint16_t>(port);
        } catch (...) { return false; }
        return true;
    }
}

bool ConnectionCode::isValid(std::string_view code) {
    std::string ip;
    uint16_t port = 0;
    return parse(code, ip, port);
}

} // namespace yunyi
