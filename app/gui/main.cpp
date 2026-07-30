#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <devguid.h>
#include "../../webview2-sdk/build/native/include/WebView2.h"
#include "../component/NetworkAutoConfig.h"
#include <wrl/event.h>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <vector>
#include <wbemidl.h>
#include <comdef.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")

using namespace Microsoft::WRL;

// 读取注册表字符串值
static std::string RegReadString(HKEY hRoot, const wchar_t* subkey, const wchar_t* valueName) {
    HKEY hKey;
    if (RegOpenKeyExW(hRoot, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return "";
    wchar_t buf[256]{};
    DWORD size = sizeof(buf);
    DWORD type = REG_SZ;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return "";
    }
    RegCloseKey(hKey);
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &result[0], len, nullptr, nullptr);
    result.resize(len - 1);  // trim null terminator
    return result;
}

/** 精简 CPU 名称 */
static std::string SimplifyCPUName(const std::string& raw) {
    std::string s = raw;
    for (auto& suffix : { "w/ Radeon Graphics", "with Radeon Graphics" }) {
        auto p = s.find(suffix);
        if (p != std::string::npos) s = s.substr(0, p);
    }
    auto dash = s.find("-Core Processor");
    if (dash != std::string::npos) {
        while (dash > 0 && s[dash - 1] != ' ') dash--;
        s = s.substr(0, dash);
    }
    for (auto& ch : { "(R)", "(TM)" }) {
        size_t p;
        while ((p = s.find(ch)) != std::string::npos) s.erase(p, 2 + (ch[2] == ')'));
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

/** 采集本机硬件信息 */
static std::string CollectDeviceInfoJSON() {
    std::ostringstream js;

    // CPU
    std::string cpu = SimplifyCPUName(RegReadString(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString"));

    // GPU
    std::string gpu;
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    if (EnumDisplayDevicesW(nullptr, 0, &dd, 0)) {
        char buf[128]{};
        WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, buf, sizeof(buf), nullptr, nullptr);
        gpu = buf;
        for (auto& prefix : { "RTX ", "GTX ", "GT ", "RX ", "Arc " }) {
            auto p = gpu.find(prefix);
            if (p != std::string::npos && p > 0) { gpu.insert(p, "<br>"); break; }
        }
    }

    // 内存：容量从 OS 获取（准确），品牌/频率从 SMBIOS 补充
    std::string memory;
    std::string pn;
    WORD spd = 0;

    // WMI 查询 Win32_PhysicalMemory：品牌、频率
    {
        IWbemLocator* pLoc = nullptr;
        IWbemServices* pSvc = nullptr;
        IEnumWbemClassObject* pEnum = nullptr;
        auto bstr = [](const wchar_t* s) { return _bstr_t(s); };
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, (LPVOID*)&pLoc);
        if (SUCCEEDED(hr) && pLoc) {
            hr = pLoc->ConnectServer(bstr(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr,
                0, nullptr, nullptr, &pSvc);
            if (SUCCEEDED(hr) && pSvc) {
                CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                hr = pSvc->ExecQuery(bstr(L"WQL"),
                    bstr(L"SELECT Manufacturer,PartNumber,Speed FROM Win32_PhysicalMemory"),
                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
                if (SUCCEEDED(hr) && pEnum) {
                    IWbemClassObject* pObj = nullptr;
                    ULONG ret = 0;
                    while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == WBEM_S_NO_ERROR && pObj) {
                        VARIANT vt;
                        VariantInit(&vt);
                        auto readStr = [&](const wchar_t* prop) -> std::string {
                            std::string s;
                            if (SUCCEEDED(pObj->Get(prop, 0, &vt, nullptr, nullptr)) && vt.vt == VT_BSTR && vt.bstrVal) {
                                int len = WideCharToMultiByte(CP_UTF8, 0, vt.bstrVal, -1, nullptr, 0, nullptr, nullptr);
                                if (len > 1) { s.resize(len - 1); WideCharToMultiByte(CP_UTF8, 0, vt.bstrVal, -1, &s[0], len, nullptr, nullptr); }
                            }
                            VariantClear(&vt);
                            return s;
                        };
                        if (pn.empty()) {
                            std::string mfr = readStr(L"Manufacturer");
                            std::string part = readStr(L"PartNumber");
                            // 跳过无效厂商名
                            if (!mfr.empty() && mfr != "Unknown" && mfr != "N/A" && mfr != "0000") {
                                pn = mfr;
                            } else if (!part.empty()) {
                                pn = part;
                            }
                            // 清洗后缀，去首尾空白
                            for (auto& suffix : { " Technology", " Semiconductor", " Electronics" }) {
                                auto pos = pn.find(suffix);
                                if (pos != std::string::npos) pn.erase(pos);
                            }
                            auto trim = [](std::string& s) {
                                s.erase(0, s.find_first_not_of(" \t\r\n"));
                                s.erase(s.find_last_not_of(" \t\r\n") + 1);
                            };
                            trim(pn);
                        }
                        if (spd == 0) {
                            VariantClear(&vt); VariantInit(&vt);
                            if (SUCCEEDED(pObj->Get(L"Speed", 0, &vt, nullptr, nullptr)) && vt.vt == VT_I4 && vt.intVal > 0)
                                spd = (WORD)vt.intVal;
                            VariantClear(&vt);
                        }
                        pObj->Release();
                    }
                }
            }
        }
        if (pEnum) pEnum->Release();
        if (pSvc) pSvc->Release();
        if (pLoc) pLoc->Release();
    }

    // 容量始终用 GetPhysicallyInstalledSystemMemory，不受硬件保留影响
    ULONGLONG totalKB = 0;
    if (GetPhysicallyInstalledSystemMemory(&totalKB) && totalKB > 0) {
        DWORDLONG totalMB = totalKB / 1024;
        DWORDLONG perGB = (totalMB + 512) / 1024;
        if (!pn.empty()) memory = pn + "<br>";
        memory += std::to_string(perGB) + " GB";
        if (spd > 0) memory += "<br>" + std::to_string(spd) + "MHz";
    }
    // 回退：GlobalMemoryStatusEx
    if (memory.empty()) {
        MEMORYSTATUSEX msx = { sizeof(msx) };
        if (GlobalMemoryStatusEx(&msx))
            memory = std::to_string((msx.ullTotalPhys + 512ULL * 1024 * 1024) / (1024ULL * 1024 * 1024)) + " GB";
    }

    // 主板
    std::string mfr = RegReadString(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardManufacturer");
    std::string mbProd = RegReadString(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardProduct");
    std::string motherboard = mfr.empty() ? mbProd : (mfr + " " + mbProd);
    while (!motherboard.empty() && (motherboard.back() == ' ' || motherboard.back() == '.')) motherboard.pop_back();

    // 硬盘：枚举物理磁盘
    std::string disk;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devData = { sizeof(devData) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); i++) {
            WCHAR friendly[256]{};
            if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_FRIENDLYNAME,
                    nullptr, (PBYTE)friendly, sizeof(friendly), nullptr)) {
                char name[256]{};
                WideCharToMultiByte(CP_UTF8, 0, friendly, -1, name, sizeof(name), nullptr, nullptr);
                if (!disk.empty()) disk += "<br>";
                disk += name;
            }
        }
        SetupDiDestroyDeviceInfoList(devInfo);
    }
    if (disk.empty()) disk = "未知";

    // 操作系统
    std::string os = RegReadString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    if (os.find("Windows 10") != std::string::npos) {
        std::string bld = RegReadString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
        if (!bld.empty() && std::stoi(bld) >= 22000)
            os.replace(os.find("Windows 10"), 10, "Windows 11");
    }

    auto esc = [](const std::string& s) -> std::string {
        std::string r;
        for (char c : s) {
            if (c == '\\') r += "\\\\";
            else if (c == '\'') r += "\\'";
            else if (c == '\n') r += "\\n";
            else if (c == '\r') r += "\\r";
            else if (c == '\t') r += "\\t";
            else r += c;
        }
        return r;
    };

    js << "if(window.onDeviceInfo)window.onDeviceInfo({"
       << "cpu:'" << esc(cpu) << "',"
       << "gpu:'" << esc(gpu) << "',"
       << "memory:'" << esc(memory) << "',"
       << "motherboard:'" << esc(motherboard) << "',"
       << "disk:'" << esc(disk) << "',"
       << "os:'" << esc(os) << "'"
       << "})";
    return js.str();
}

/** 采集实时硬件占用，返回 JSON 字符串（每 3s 调用） */
static std::string CollectDeviceUsageJSON() {
    // CPU: GetSystemTimes 计算使用率
    static FILETIME prevIdle{}, prevKernel{}, prevUser{};
    static bool firstCall = true;
    int cpuPct = 0;
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        if (!firstCall) {
            auto toU64 = [](const FILETIME& ft) -> ULONGLONG {
                return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            };
            ULONGLONG idleDiff = toU64(idle) - toU64(prevIdle);
            ULONGLONG kernelDiff = toU64(kernel) - toU64(prevKernel);
            ULONGLONG userDiff = toU64(user) - toU64(prevUser);
            ULONGLONG totalDiff = kernelDiff + userDiff;
            if (totalDiff > 0) {
                cpuPct = static_cast<int>(100 - (idleDiff * 100 / totalDiff));
                if (cpuPct < 0) cpuPct = 0;
                if (cpuPct > 100) cpuPct = 100;
            }
        }
        prevIdle = idle; prevKernel = kernel; prevUser = user;
        firstCall = false;
    }

    // 内存: GlobalMemoryStatusEx
    int memPct = 0;
    MEMORYSTATUSEX msx = { sizeof(msx) };
    if (GlobalMemoryStatusEx(&msx)) {
        memPct = static_cast<int>(msx.dwMemoryLoad);
    }

    // 硬盘: C: 使用率
    int diskPct = 0;
    ULARGE_INTEGER freeBytes, totalBytes;
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &totalBytes, nullptr) && totalBytes.QuadPart > 0) {
        diskPct = static_cast<int>(100 - (freeBytes.QuadPart * 100 / totalBytes.QuadPart));
    }

    std::ostringstream js;
    js << "if(window.onDeviceUsage)window.onDeviceUsage({"
       << "cpu:" << cpuPct << ","
       << "memory:" << memPct << ","
       << "disk:" << diskPct
       << "})";
    return js.str();
}

/** 从本机网卡获取公网 IPv6 地址，失败再尝试 IPv4 */
static std::string DetectPublicIP() {
    // 1. 先从本机网卡获取公网 IPv6（非临时、非本地链路、非回环）
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        nullptr, nullptr, &bufLen);
    if (bufLen > 0) {
        auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(new char[bufLen]);
        if (GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                nullptr, addrs, &bufLen) == NO_ERROR) {
            for (auto* a = addrs; a; a = a->Next) {
                for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                    auto* a6 = reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr);
                    if (a6->sin6_family != AF_INET6) continue;
                    char ip[INET6_ADDRSTRLEN]{};
                    inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
                    std::string ipStr(ip);
                    // 跳过 loopback / link-local / multicast / 临时地址
                    if (ipStr == "::1") continue;
                    if (ipStr.find("fe80:") == 0) continue;
                    if (ipStr.find("ff") == 0) continue;
                    // 跳过临时 IPv6（通常后缀随机）
                    // 优先取全球单播地址（2xxx: 或 3xxx: 开头）
                    if (ipStr[0] == '2' || ipStr[0] == '3') {
                        delete[] reinterpret_cast<char*>(addrs);
                        return ipStr;
                    }
                }
            }
        }
        delete[] reinterpret_cast<char*>(addrs);
    }
    return "";  // 全部失败，回退 localhost
}

static ICoreWebView2Environment*  g_env     = nullptr;
static ICoreWebView2Controller*   g_ctrl    = nullptr;
static ICoreWebView2*             g_webview = nullptr;
static PROCESS_INFORMATION        g_relayPI = {};
static PROCESS_INFORMATION        g_hostPI  = {};

static void KillAllServers() {
    if (g_relayPI.hProcess) {
        TerminateProcess(g_relayPI.hProcess, 0);
        CloseHandle(g_relayPI.hProcess);
        if (g_relayPI.hThread) CloseHandle(g_relayPI.hThread);
        g_relayPI = {};
    }
    if (g_hostPI.hProcess) {
        TerminateProcess(g_hostPI.hProcess, 0);
        CloseHandle(g_hostPI.hProcess);
        if (g_hostPI.hThread) CloseHandle(g_hostPI.hThread);
        g_hostPI = {};
    }
}

static void StartAllServers() {
    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (slash) *(slash + 1) = 0;

    // 自动检测公网 IP（异步，不阻塞 GUI 启动）
    std::string publicIP = DetectPublicIP();
    if (publicIP.empty()) {
        publicIP = "127.0.0.1";  // 检测失败先用本地，用户可在 WebUI 手动改
    }
    WCHAR ipWide[128];
    MultiByteToWideChar(CP_UTF8, 0, publicIP.c_str(), -1, ipWide, 128);

    // ── 启动 Relay 进程（中继服务，WebUI 端口 8080）──
    {
        WCHAR cmd[MAX_PATH + 256];
        wcscpy_s(cmd, dir);
        wcscat_s(cmd, L"云驿.exe --relay --http-port 8080 --public-ip ");
        wcscat_s(cmd, ipWide);
        wcscat_s(cmd, L" --psk yunyi-beta-key-2026");

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
        if (pi.hProcess) {
            g_relayPI = pi;
        }
    }

    // ── 启动 Host 进程（房主端，HTTP 端口 8081，空闲模式）──
    {
        WCHAR cmd[MAX_PATH + 256];
        wcscpy_s(cmd, dir);
        wcscat_s(cmd, L"云驿.exe --host --http-port 8081 --public-ip ");
        wcscat_s(cmd, ipWide);
        wcscat_s(cmd, L" --psk yunyi-beta-key-2026 --no-auto-connect");

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
        if (pi.hProcess) {
            g_hostPI = pi;
        }
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        KillAllServers();
        if (g_webview) g_webview->Release();
        if (g_ctrl)    g_ctrl->Release();
        if (g_env)     g_env->Release();
        PostQuitMessage(0);
        return 0;
    case WM_NCCALCSIZE:
        if (wp == TRUE) return 0;
        break;
    case WM_SIZE:
        if (g_ctrl) { RECT r; GetClientRect(hwnd, &r); g_ctrl->put_Bounds(r); }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = 1100;
        mmi->ptMinTrackSize.y = 700;
        mmi->ptMaxTrackSize.x = 1500;
        mmi->ptMaxTrackSize.y = 960;
        return 0;
    }
    case WM_NCHITTEST: {
        const int border = 8;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc; GetWindowRect(hwnd, &rc);
        pt.x -= rc.left; pt.y -= rc.top;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        bool L = pt.x < border, R = pt.x > w - border;
        bool T = pt.y < border, B = pt.y > h - border;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        return HTCLIENT;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

HRESULT OnWebViewCreated(HWND hwnd, ICoreWebView2Controller* ctrl) {
    g_ctrl = ctrl;
    g_ctrl->AddRef();

    RECT r;
    GetClientRect(hwnd, &r);
    ctrl->put_Bounds(r);
    ctrl->put_IsVisible(TRUE);

    ctrl->get_CoreWebView2(&g_webview);

    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                PWSTR msg;
                a->TryGetWebMessageAsString(&msg);
                if (!msg) { CoTaskMemFree(msg); return S_OK; }
                if (wcscmp(msg, L"drag") == 0) {
                    ReleaseCapture();
                    POINT pt; GetCursorPos(&pt);
                    SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
                } else if (wcscmp(msg, L"start_net_check") == 0) {
                    // 用户点击"启动服务器" → 运行网络检测
                    yunyi::NetworkAutoConfig::run(
                        [wv = g_webview](const std::string& step, bool ok, const std::string& detail) {
                            std::ostringstream js;
                            js << "if(window.onCheckStep)window.onCheckStep({"
                               << "step:'" << step << "',ok:" << (ok ? "true" : "false")
                               << ",detail:'" << detail << "'})";
                            std::string jsStr = js.str();
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, nullptr, 0);
                            std::wstring wjs(wlen, 0);
                            MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, &wjs[0], wlen);
                            wv->ExecuteScript(wjs.c_str(), nullptr);
                        },
                        [wv = g_webview](const yunyi::NetStatus& ns) {
                            std::ostringstream js;
                            js << "if(window.onNetworkStatus)window.onNetworkStatus({"
                               << "publicIPv6:'" << ns.publicIPv6 << "',"
                               << "hasPublicIPv6:" << (ns.hasPublicIPv6 ? "true" : "false") << ","
                               << "selfTestOk:" << (ns.selfTestOk ? "true" : "false") << ","
                               << "statusText:'" << ns.statusText << "',"
                               << "statusLevel:'" << ns.statusLevel << "'})";
                            std::string jsStr = js.str();
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, nullptr, 0);
                            std::wstring wjs(wlen, 0);
                            MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, &wjs[0], wlen);
                            wv->ExecuteScript(wjs.c_str(), nullptr);
                        }
                    );
                } else if (wcscmp(msg, L"minimize") == 0) {
                    ShowWindow(hwnd, SW_MINIMIZE);
                } else if (wcscmp(msg, L"close") == 0) {
                    PostQuitMessage(0);
                }
                CoTaskMemFree(msg);
                return S_OK;
            }).Get(), nullptr);

    g_webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                PWSTR uri;
                a->get_Uri(&uri);
                if (wcscmp(uri, L"yunyi://close") == 0) {
                    PostQuitMessage(0);
                    a->put_Cancel(TRUE);
                }
                CoTaskMemFree(uri);
                return S_OK;
            }).Get(), nullptr);

    g_webview->AddScriptToExecuteOnDocumentCreated(
        L"var s=document.createElement('style');s.id='yunyi-base';"
        L"s.textContent='html,body{padding:0!important;margin:0!important;"
        L"background:white!important;overflow:hidden!important}"
        L".app{box-shadow:none!important}';"
        L"document.head.appendChild(s);",
        nullptr);

    g_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2* wv, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                wv->ExecuteScript(
                    L"var s=document.getElementById('yunyi-scrollbar');"
                    L"if(!s){s=document.createElement('style');s.id='yunyi-scrollbar';"
                    L"document.head.appendChild(s);}"
                    L"s.textContent='::-webkit-scrollbar{width:0!important;"
                    L"height:0!important;display:none!important}"
                    L"*{scrollbar-width:none!important}'",
                    nullptr);
                // 推送硬件设备信息
                std::string diJson = CollectDeviceInfoJSON();
                int dlen = MultiByteToWideChar(CP_UTF8, 0, diJson.c_str(), -1, nullptr, 0);
                std::wstring dwjs(dlen, 0);
                MultiByteToWideChar(CP_UTF8, 0, diJson.c_str(), -1, &dwjs[0], dlen);
                wv->ExecuteScript(dwjs.c_str(), nullptr);
                return S_OK;
            }).Get(), nullptr);

    // 直接加载主界面
    g_webview->Navigate(L"http://127.0.0.1:8080/");

    // 后台线程：每 5 分钟尝试连服务器（不影响正常使用）
    std::thread([wv = g_webview]() {
        while (true) {
            bool ok = false;
            HINTERNET hS = WinHttpOpen(L"YunYi/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (hS) {
                HINTERNET hC = WinHttpConnect(hS, L"mc.qinnai.xyz", 2885, 0);
                if (hC) {
                    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", L"/api/users/online",
                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                    if (hR) {
                        DWORD t = 3000;
                        WinHttpSetOption(hR, WINHTTP_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
                        WinHttpSetOption(hR, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
                        if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                            WinHttpReceiveResponse(hR, nullptr)) {
                            ok = true;
                        }
                        WinHttpCloseHandle(hR);
                    }
                    WinHttpCloseHandle(hC);
                }
                WinHttpCloseHandle(hS);
            }
            // 推送到 titlebar dot
            std::string js = std::string("var d=document.getElementById('net-dot');")
                + "if(d)d.className='dot " + (ok ? "on" : "off") + "';"
                + "var t=document.getElementById('net-status');"
                + "if(t)t.textContent='" + (ok ? "服务器已连接" : "服务器未连接") + "';";
            int wlen = MultiByteToWideChar(CP_UTF8, 0, js.c_str(), -1, nullptr, 0);
            std::wstring wjs(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, js.c_str(), -1, &wjs[0], wlen);
            wv->ExecuteScript(wjs.c_str(), nullptr);
            // 5 分钟后重试
            Sleep(300000);
        }
    }).detach();

    // 后台线程：网络检测 + 防火墙放行 + 自检（逐步推送进度）
    yunyi::NetworkAutoConfig::run(
        // 每步回调
        [wv = g_webview](const std::string& step, bool ok, const std::string& detail) {
            std::ostringstream js;
            js << "if(window.onCheckStep)window.onCheckStep({"
               << "step:'" << step << "',"
               << "ok:" << (ok ? "true" : "false") << ","
               << "detail:'" << detail << "'"
               << "})";
            std::string jsStr = js.str();
            int wlen = MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, nullptr, 0);
            std::wstring wjs(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, &wjs[0], wlen);
            wv->ExecuteScript(wjs.c_str(), nullptr);
        },
        // 最终结果
        [wv = g_webview](const yunyi::NetStatus& ns) {
            std::ostringstream js;
            js << "if(window.onNetworkStatus)window.onNetworkStatus({"
               << "publicIPv6:'" << ns.publicIPv6 << "',"
               << "hasPublicIPv6:" << (ns.hasPublicIPv6 ? "true" : "false") << ","
               << "isCGNAT:" << (ns.isCGNAT ? "true" : "false") << ","
               << "firewallOk:" << (ns.firewallOk ? "true" : "false") << ","
               << "selfTestOk:" << (ns.selfTestOk ? "true" : "false") << ","
               << "statusText:'" << ns.statusText << "',"
               << "statusLevel:'" << ns.statusLevel << "'"
               << "})";
            std::string jsStr = js.str();
            int wlen = MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, nullptr, 0);
            std::wstring wjs(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, &wjs[0], wlen);
            wv->ExecuteScript(wjs.c_str(), nullptr);
        }
    );

    // 后台线程：每 3 秒推送硬件占用（CPU/内存/硬盘）
    // 首次 2 秒后额外推送一次设备信息，防止 NavigationCompleted 时 JS 未就绪
    std::thread([wv = g_webview]() {
        bool first = true;
        while (true) {
            Sleep(first ? 2000 : 3000);
            if (first) {
                // 保底：首次再推一次设备信息
                std::string di = CollectDeviceInfoJSON();
                int dlen = MultiByteToWideChar(CP_UTF8, 0, di.c_str(), -1, nullptr, 0);
                std::wstring dw(dlen, 0);
                MultiByteToWideChar(CP_UTF8, 0, di.c_str(), -1, &dw[0], dlen);
                wv->ExecuteScript(dw.c_str(), nullptr);
                first = false;
            }
            std::string jsStr = CollectDeviceUsageJSON();
            int wlen = MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, nullptr, 0);
            std::wstring wjs(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, jsStr.c_str(), -1, &wjs[0], wlen);
            wv->ExecuteScript(wjs.c_str(), nullptr);
        }
    }).detach();

    return S_OK;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    StartAllServers();
    Sleep(1500);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = static_cast<HICON>(LoadImageW(hInst, L"icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE));
    wc.hIconSm       = static_cast<HICON>(LoadImageW(hInst, L"icon.ico", IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
    wc.hbrBackground = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    wc.lpszClassName = L"YunYiClass";
    RegisterClassExW(&wc);

    int winW = 1500;
    int winH = 960;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED, L"YunYiClass", L"云驿",
        WS_POPUP | WS_THICKFRAME,
        (sw - winW) / 2, (sh - winH) / 2, winW, winH,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { KillAllServers(); CoUninitialize(); return 1; }

    DWM_WINDOW_CORNER_PREFERENCE cp = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cp, sizeof(cp));
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                g_env = env;
                g_env->AddRef();
                return env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT, ICoreWebView2Controller* ctrl) -> HRESULT {
                            return OnWebViewCreated(hwnd, ctrl);
                        }).Get());
            }).Get());

    if (FAILED(hr)) {
        MessageBoxW(hwnd,
            L"未检测到 Microsoft Edge WebView2 运行时。\n"
            L"请下载安装：https://go.microsoft.com/fwlink/p/?LinkId=2124702",
            L"云驿 - 启动失败", MB_ICONERROR);
        KillAllServers();
        DestroyWindow(hwnd);
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, nShow);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    KillAllServers();
    CoUninitialize();
    return 0;
}
