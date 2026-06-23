#pragma once

#include <string>
#include <string_view>
#include <format>
#include <iostream>
#include <fstream>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdio>

namespace opencode::log {

// =============================================================================
// 级别
// =============================================================================

enum class Level : int {
    trace = 0,
    debug = 1,
    info  = 2,
    warn  = 3,
    error = 4,
    off   = 5
};

inline std::string_view level_str(Level lv) {
    switch (lv) {
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info:  return "INFO ";
        case Level::warn:  return "WARN ";
        case Level::error: return "ERROR";
        default:           return "?????";
    }
}

// =============================================================================
// 全局 Logger
// =============================================================================

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(Level lv) { level_ = lv; }
    Level level() const { return level_; }

    void set_file(const std::string& path) {
        std::lock_guard lock(mutex_);
        if (file_.is_open()) file_.close();
        file_.open(path, std::ios::out | std::ios::app);
    }

    void log(Level lv, std::string_view file, int line, std::string msg) {
        if (lv < level_) return;

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100;

        char time_buf[16];
        std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&time));

        std::string line_str = std::format("[{}.{:03}] [{}] [t{}] {}:{}  {}\n",
            time_buf, ms.count(), level_str(lv), tid,
            file, line, msg);

        std::lock_guard lock(mutex_);
        std::cerr << line_str;
        if (file_.is_open()) file_ << line_str << std::flush;
    }

private:
    Logger() {
        const char* env = std::getenv("OPENCODE_LOG_LEVEL");
        if (env) {
            std::string s(env);
            if (s == "trace") level_ = Level::trace;
            else if (s == "debug") level_ = Level::debug;
            else if (s == "info")  level_ = Level::info;
            else if (s == "warn")  level_ = Level::warn;
            else if (s == "error") level_ = Level::error;
            else if (s == "off")   level_ = Level::off;
        }
        const char* f = std::getenv("OPENCODE_LOG_FILE");
        if (f) set_file(f);
    }

    Level level_ = Level::info;
    std::mutex mutex_;
    std::ofstream file_;
};

} // namespace opencode::log

// =============================================================================
// 宏
// =============================================================================

// 根据编译期 LOG_ACTIVE_LEVEL 裁剪低级别日志
#ifndef LOG_ACTIVE_LEVEL
#define LOG_ACTIVE_LEVEL 0  // trace = 0, 默认全部
#endif

#define LOG_TRACE(fmt, ...) \
    do { if constexpr ((int)opencode::log::Level::trace >= LOG_ACTIVE_LEVEL) \
         opencode::log::Logger::instance().log(opencode::log::Level::trace, __FILE__, __LINE__, std::format(fmt, ##__VA_ARGS__)); } while(0)

#define LOG_DEBUG(fmt, ...) \
    do { if constexpr ((int)opencode::log::Level::debug >= LOG_ACTIVE_LEVEL) \
         opencode::log::Logger::instance().log(opencode::log::Level::debug, __FILE__, __LINE__, std::format(fmt, ##__VA_ARGS__)); } while(0)

#define LOG_INFO(fmt, ...) \
    do { opencode::log::Logger::instance().log(opencode::log::Level::info,  __FILE__, __LINE__, std::format(fmt, ##__VA_ARGS__)); } while(0)

#define LOG_WARN(fmt, ...) \
    do { opencode::log::Logger::instance().log(opencode::log::Level::warn,  __FILE__, __LINE__, std::format(fmt, ##__VA_ARGS__)); } while(0)

#define LOG_ERROR(fmt, ...) \
    do { opencode::log::Logger::instance().log(opencode::log::Level::error, __FILE__, __LINE__, std::format(fmt, ##__VA_ARGS__)); } while(0)
