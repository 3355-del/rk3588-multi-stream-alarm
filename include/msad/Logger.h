/*
 * Logger.h — 轻量线程安全日志模块
 *
 * 特性：
 *   - 单例，全局唯一日志输出
 *   - 线程安全（互斥锁保护输出）
 *   - 分级：DEBUG / INFO / WARN / ERROR
 *   - 输出到 stdout（可扩展为文件）
 */
#ifndef MULTI_STREAM_LOGGER_H
#define MULTI_STREAM_LOGGER_H

#include <cstdio>
#include <atomic>
#include <ctime>
#include <string>
#include <mutex>

namespace msad {

enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3
};

class Logger {
public:
    /* 获取单例 */
    static Logger& instance();

    /* 设置最低输出级别（低于该级别的不打印） */
    void setLevel(LogLevel level) { level_ = level; }

    /* 打印一条日志，线程安全 */
    void log(LogLevel level, const std::string& tag, const std::string& msg);

    /* 便捷静态方法 */
    static void debug(const std::string& tag, const std::string& msg) {
        instance().log(LogLevel::DEBUG, tag, msg);
    }
    static void info(const std::string& tag, const std::string& msg) {
        instance().log(LogLevel::INFO, tag, msg);
    }
    static void warn(const std::string& tag, const std::string& msg) {
        instance().log(LogLevel::WARN, tag, msg);
    }
    static void error(const std::string& tag, const std::string& msg) {
        instance().log(LogLevel::ERROR, tag, msg);
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /* 取当前本地时间字符串 [HH:MM:SS] */
    static std::string nowTime();

    std::mutex mtx_;        /* 保护 stdout 输出 */
    std::atomic<LogLevel> level_{LogLevel::DEBUG};
};

} // namespace msad

#endif // MULTI_STREAM_LOGGER_H
