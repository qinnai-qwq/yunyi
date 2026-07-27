#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include "../../webview2-sdk/build/native/include/WebView2.h"
#include "../component/NetworkAutoConfig.h"
#include <wrl/event.h>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")

using namespace Microsoft::WRL;

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
