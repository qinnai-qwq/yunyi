/**
 * @file Logger.h
 * @brief 全局日志工具 —— 写入 .log 文件（线程安全，带时间戳）
 *
 * GUI（WebView2）无控制台，后台进程状态无法从 stdout 看到。
 * 统一用 CC_LOG 写入日志文件，便于定位问题。
 *
 * 用法（无需初始化，首次 CC_LOG 自动落到当前目录 yunyi.log）：
 *   CC_LOG("Director: room created id=" + std::to_string(id));
 *
 * 需要指定路径时可显式调用：
 *   yunyi::Logger::instance().init(L"E:/yunyi/yunyi.log");
 */
#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <ctime>

namespace yunyi {

/**
 * 全局日志单例。
 * - 线程安全：log() 内部加锁
 * - 追加写文件，不覆盖历史
 * - 文件打不开时静默降级（不影响业务）
 */
class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    /** 覆盖默认日志文件路径（宽字符版本，Windows 用） */
    void init(const wchar_t* filePath) {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_ofs.is_open()) _ofs.close();
        _ofs.open(filePath, std::ios::out | std::ios::app);
    }

    /** 覆盖默认日志文件路径（窄字符版本） */
    void init(const char* filePath) {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_ofs.is_open()) _ofs.close();
        _ofs.open(filePath, std::ios::out | std::ios::app);
    }

    /** 写入一条日志：`[YYYY-MM-DD HH:MM:SS] msg` */
    void log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(_mtx);
        // 未显式 init 时自动落到当前目录的 yunyi.log
        if (!_ofs.is_open()) {
            _ofs.open(L"yunyi.log", std::ios::out | std::ios::app);
        }
        if (!_ofs.is_open()) return;

        char ts[32];
        std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        _ofs << '[' << ts << "] " << msg << '\n';
        _ofs.flush();
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex _mtx;
    std::ofstream _ofs;
};

} // namespace yunyi

/** 全局日志宏：写入 yunyi.log */
#define CC_LOG(msg) ::yunyi::Logger::instance().log(msg)
