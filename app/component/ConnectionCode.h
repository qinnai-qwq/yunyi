/**
 * @file ConnectionCode.h
 * @brief 连接码生成/解析 —— 格式 "公网IP:端口"
 *
 * 房主创建房间后获得连接码（如 "1.2.3.4:40123"），
 * 分享给玩家后，玩家在 MC 中输入即可连接。
 */
#pragma once
#include <cstdint>
#include <string>
namespace yunyi {

/**
 * @class ConnectionCode
 * @brief 连接码工具类（纯静态方法）
 *
 * 连接码格式: "IPv4:Port"，例如 "192.168.1.100:40005"。
 */
class ConnectionCode {
public:
    /**
     * @brief 生成连接码
     * @param ip 公网 IP 地址
     * @param port 端口号
     * @return "IP:Port" 格式字符串
     */
    static std::string generate(const std::string& ip, uint16_t port);

    /**
     * @brief 解析连接码
     * @param code 连接码字符串
     * @param[out] ip 解析出的 IP 地址
     * @param[out] port 解析出的端口号
     * @return true 解析成功，false 格式错误
     */
    static bool parse(std::string_view code, std::string& ip, uint16_t& port);

    /**
     * @brief 验证连接码格式是否合法
     * @param code 连接码字符串
     * @return true 格式合法
     */
    static bool isValid(std::string_view code);
};

}
