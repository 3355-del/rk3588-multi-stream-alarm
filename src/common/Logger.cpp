/*
 * Logger.cpp — 日志模块实现
 */
#include "msad/Logger.h"

namespace msad {

Logger& Logger::instance() {
    static Logger s_instance;   /* C++11 起线程安全的静态局部单例 */
    return s_instance;
}

std::string Logger::nowTime() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);        /* MSVC/Windows 线程安全版本 */
#else
    localtime_r(&t, &tm);        /* POSIX 线程安全版本 */
#endif
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& msg) {
    if (level < level_.load()) return;   /* 低于阈值的跳过 */

    static const char* kLevelNames[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    int idx = static_cast<int>(level);
    if (idx < 0 || idx > 3) idx = 0;

    std::lock_guard<std::mutex> lock(mtx_);
    std::printf("[%s][%s][%s] %s\n",
                nowTime().c_str(), kLevelNames[idx], tag.c_str(), msg.c_str());
    std::fflush(stdout);
}

} // namespace msad
