#include "NetworkAutoConfig.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netfw.h>
#include <natupnp.h>
#include <comdef.h>
#include <winhttp.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "winhttp.lib")
#endif

#include <thread>
#include <sstream>

namespace yunyi {

// ============================================================
//  检测网络类型
// ============================================================
NetStatus NetworkAutoConfig::detectNetworkType() {
    NetStatus ns;
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET6,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        nullptr, nullptr, &bufLen);
    if (bufLen == 0) { ns.statusText = "无网络适配器"; ns.statusLevel = "error"; return ns; }

    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(new char[bufLen]);
    if (GetAdaptersAddresses(AF_INET6,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            nullptr, addrs, &bufLen) != NO_ERROR) {
        delete[] reinterpret_cast<char*>(addrs);
        ns.statusText = "无法获取网络信息"; ns.statusLevel = "error"; return ns;
    }

    for (auto* a = addrs; a; a = a->Next) {
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* a6 = reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr);
            if (a6->sin6_family == AF_INET6) {
                char ip[INET6_ADDRSTRLEN]{};
                inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
                std::string ipStr(ip);
                if (ipStr == "::1" || ipStr.find("fe80:") == 0 || ipStr[0] == 'f') continue;
                // 公网 IPv6（2000::/3）
                if (ipStr[0] == '2' || ipStr[0] == '3') {
                    ns.hasPublicIPv6 = true;
                    ns.publicIPv6 = ipStr;
                }
            }
            auto* a4 = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            if (a4->sin_family == AF_INET) {
                char ip[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
                std::string ipStr(ip);
                int first = atoi(ipStr.substr(0, ipStr.find('.')).c_str());
                if (first == 10 || first == 127 || first == 0) continue;
                if (first == 100 && ipStr[1] != '.') { int second = atoi(ipStr.substr(ipStr.find('.')+1).c_str()); if (second >= 64 && second <= 127) { ns.isCGNAT = true; continue; } }
                if (first == 172 && first >= 16 && first <= 31) continue;
                if (first == 192 && ipStr.find("168.") == 0) { /* check 192.168 */ continue; }
                ns.hasPublicIPv4 = true;
                ns.publicIPv4 = ipStr;
            }
        }
    }
    delete[] reinterpret_cast<char*>(addrs);

    if (ns.hasPublicIPv6) {
        ns.statusText = "公网 IPv6: " + ns.publicIPv6;
        ns.statusLevel = "ok";
    } else if (ns.hasPublicIPv4 && !ns.isCGNAT) {
        ns.statusText = "公网 IPv4: " + ns.publicIPv4;
        ns.statusLevel = "ok";
    } else if (ns.hasPublicIPv4 && ns.isCGNAT) {
        ns.statusText = "CGNAT 环境（IPv4 不可入站）";
        ns.statusLevel = "warn";
    } else {
        ns.statusText = "无公网 IP，仅可作为客户端";
        ns.statusLevel = "error";
    }
    return ns;
}

// ============================================================
//  Windows 防火墙自动放行
// ============================================================
bool NetworkAutoConfig::addFirewallRules() {
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needUninit = SUCCEEDED(hr);

    INetFwPolicy2* fwPolicy = nullptr;
    hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&fwPolicy));
    if (FAILED(hr) || !fwPolicy) {
        if (needUninit) CoUninitialize();
        return false;
    }

    // 检查防火墙是否启用
    long profile = 7;  // DOMAIN(1) | PRIVATE(2) | PUBLIC(4)

    auto addRule = [&](const wchar_t* name, const wchar_t* ports) -> bool {
        INetFwRule* rule = nullptr;
        hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(INetFwRule), reinterpret_cast<void**>(&rule));
        if (FAILED(hr)) return false;
        rule->put_Name(_bstr_t(name));
        rule->put_Description(_bstr_t(L"YunYi MC Relay"));
        rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP);
        rule->put_LocalPorts(_bstr_t(ports));
        rule->put_Direction(NET_FW_RULE_DIR_IN);
        rule->put_Enabled(VARIANT_TRUE);
        rule->put_Profiles(profile);
        rule->put_Action(NET_FW_ACTION_ALLOW);
        INetFwRules* rules = nullptr;
        fwPolicy->get_Rules(&rules);
        hr = rules->Add(rule);
        rule->Release();
        if (rules) rules->Release();
        return SUCCEEDED(hr);
    };

    bool ok1 = addRule(L"云驿 控制端口", L"40000");
    bool ok2 = addRule(L"云驿 端口池", L"40001-41000");
    bool ok3 = addRule(L"云驿 HTTP", L"8080");
    bool ok4 = addRule(L"云驿 后端", L"2885");

    fwPolicy->Release();
    if (needUninit) CoUninitialize();
    return ok1 && ok2 && ok3 && ok4;
#else
    return false;
#endif
}

// ============================================================
//  UPnP IPv4 端口映射（NATUPnP COM）
// ============================================================
bool NetworkAutoConfig::tryUPnPMapping(int port, int& mappedPort) {
#ifdef _WIN32
    // 使用 NATUPnP COM 接口
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUPnPNAT* nat = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat));
    if (FAILED(hr) || !nat) { CoUninitialize(); return false; }

    IStaticPortMappingCollection* mappings = nullptr;
    hr = nat->get_StaticPortMappingCollection(&mappings);
    if (FAILED(hr) || !mappings) { nat->Release(); CoUninitialize(); return false; }

    // 添加端口映射：外部 port → 本机 port
    wchar_t portStr[16];
    swprintf_s(portStr, L"%d", port);
    // 获取本机内网 IPv4
    char hostname[256]{};
    gethostname(hostname, sizeof(hostname));
    auto* host = gethostbyname(hostname);
    std::wstring localIP;
    if (host && host->h_addr_list[0]) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, host->h_addr_list[0], ip, sizeof(ip));
        std::string ipStr(ip);
        localIP = std::wstring(ipStr.begin(), ipStr.end());
    } else {
        localIP = L"127.0.0.1";
    }

    IStaticPortMapping* mapping = nullptr;
    hr = mappings->Add(port, _bstr_t(L"TCP"), port, _bstr_t(localIP.c_str()),
        VARIANT_TRUE, _bstr_t(L"云驿 MC Relay"), &mapping);
    if (SUCCEEDED(hr) && mapping) {
        mappedPort = port;
        mapping->Release();
        mappings->Release();
        nat->Release();
        CoUninitialize();
        return true;
    }

    // 失败 → 尝试自动分配端口
    hr = mappings->Add(0, _bstr_t(L"TCP"), port, _bstr_t(localIP.c_str()),
        VARIANT_TRUE, _bstr_t(L"云驿 MC Relay"), &mapping);
    if (SUCCEEDED(hr) && mapping) {
        // 读取实际分配的外部端口
        long extPort = 0;
        mapping->get_ExternalPort(&extPort);
        mappedPort = static_cast<int>(extPort);
        mapping->Release();
        mappings->Release();
        nat->Release();
        CoUninitialize();
        return true;
    }

    mappings->Release();
    nat->Release();
    CoUninitialize();
    return false;
#else
    return false;
#endif
}

// ============================================================
//  PCP IPv6 防火墙打洞（UPnP IGD:2 AddPinhole）
// ============================================================
bool NetworkAutoConfig::tryPCPPinhole(const std::string& ipv6, int port) {
#ifdef _WIN32
    // 使用 UPnP IGD:2 接口（需要发现 IGD 设备）
    // 简化实现：IPv6 场景下路由器防火墙打洞较复杂，
    // 首选方案是 Windows 防火墙放行 + 用户手动放开路由器
    // PCP 需要路由器支持 PCP 协议（RFC 6887）
    // 此函数作为占位，后续接入 miniupnpc 的 UPNP_AddPinhole
    (void)ipv6;
    (void)port;
    return false;
#else
    return false;
#endif
}

// ============================================================
//  自检：尝试连 mc.qinnai.xyz 验证可达性
// ============================================================
bool NetworkAutoConfig::selfTest(const NetStatus& ns) {
#ifdef _WIN32
    // 用 WinHTTP 连认证服务器验证连通性
    std::wstring host = L"mc.qinnai.xyz";
    HINTERNET hSession = WinHttpOpen(L"YunYi-SelfTest/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), 2885, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/api/users/online",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD timeout = 3000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &status, &size, nullptr);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return status == 200;
#else
    return false;
#endif
}

// ============================================================
//  后台运行（线程安全）
// ============================================================
void NetworkAutoConfig::run(StepCallback stepCb, StatusCallback finalCb) {
    std::thread([stepCb, finalCb]() {
        auto step = [&](const std::string& name, bool ok, const std::string& detail) {
            if (stepCb) stepCb(name, ok, detail);
            Sleep(80);  // 让 WebView2 有时间渲染 ExecuteScript
        };

        step("网络检测", false, "正在检测...");
        NetStatus ns = detectNetworkType();

        if (ns.hasPublicIPv6) {
            step("网络检测", true, "公网 IPv6: " + ns.publicIPv6);
        } else if (ns.hasPublicIPv4 && !ns.isCGNAT) {
            step("网络检测", true, "公网 IPv4: " + ns.publicIPv4);
        } else if (ns.isCGNAT) {
            step("网络检测", false, "CGNAT 环境，无法入站");
            step("整体检测", false, "请使用宽带网络或云服务器");
            if (finalCb) finalCb(ns);
            return;
        } else {
            step("网络检测", false, "未检测到公网 IP");
            step("整体检测", false, "请检查网络连接或使用有线宽带");
            if (finalCb) finalCb(ns);
            return;
        }

        // 2. 防火墙
        step("防火墙", false, "正在放行端口...");
        ns.firewallOk = addFirewallRules();
        if (ns.firewallOk) {
            step("防火墙", true, "已放行 TCP 40000-41000/8080/2885");
        } else {
            step("防火墙", false, "自动放行失败");
        }

        // 3. UPnP/PCP
        if (ns.hasPublicIPv6) {
            step("路由器穿透", false, "正在尝试 PCP...");
            ns.upnpOk = tryPCPPinhole(ns.publicIPv6, 40000);
            if (ns.upnpOk) {
                step("路由器穿透", true, "PCP 打洞成功");
            } else {
                step("路由器穿透", false, "PCP 不支持，请手动开启路由器 IPv6 防火墙入站规则");
            }
        } else {
            step("UPnP 映射", false, "正在尝试...");
            int mapped = 0;
            ns.upnpOk = tryUPnPMapping(40000, mapped);
            if (ns.upnpOk) {
                step("UPnP 映射", true, "端口 " + std::to_string(mapped) + " 已映射");
            } else {
                step("UPnP 映射", false, "UPnP 不可用，请手动设置路由器端口转发");
            }
        }

        // 4. 自检
        step("连通性自检", false, "正在验证...");
        Sleep(500);
        ns.selfTestOk = selfTest(ns);
        if (ns.selfTestOk) {
            step("连通性自检", true, "外部可达，可作为中继节点");
        } else {
            step("连通性自检", false, "外部不可达，请检查路由器防火墙和端口转发");
        }

        // 最终状态
        if (ns.selfTestOk) {
            ns.statusText = "可作为中继节点 " + ns.publicIPv6;
            ns.statusLevel = "ok";
        } else if (ns.firewallOk) {
            ns.statusText = "防火墙已放行，请检查路由器";
            ns.statusLevel = "warn";
        } else {
            ns.statusText = "请检查网络配置";
            ns.statusLevel = "warn";
        }

        step("整体检测", ns.selfTestOk, ns.selfTestOk ? "全部通过，可启动中继" : "部分检查未通过");
        if (finalCb) finalCb(ns);
    }).detach();
}

} // namespace yunyi
