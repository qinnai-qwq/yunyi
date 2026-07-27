#pragma once
#include <string>
#include <functional>
#include <atomic>

namespace yunyi {

struct NetStatus {
    bool hasPublicIPv6 = false;
    bool hasPublicIPv4 = false;
    bool isCGNAT = false;
    std::string publicIPv6;
    std::string publicIPv4;
    bool firewallOk = false;
    bool upnpOk = false;
    bool selfTestOk = false;
    std::string statusText;   // 给 WebUI 显示
    std::string statusLevel;  // "ok" | "warn" | "error"
};

class NetworkAutoConfig {
public:
    using StatusCallback = std::function<void(const NetStatus&)>;
    using StepCallback = std::function<void(const std::string& step, bool ok, const std::string& detail)>;

    // 后台线程运行（不阻塞主线程），cb 每步回调
    static void run(StepCallback stepCb, StatusCallback finalCb);

private:
    static NetStatus detectNetworkType();
    static bool addFirewallRules();
    static bool tryUPnPMapping(int port, int& mappedPort);
    static bool tryPCPPinhole(const std::string& ipv6, int port);
    static bool selfTest(const NetStatus& ns);
};

} // namespace yunyi
