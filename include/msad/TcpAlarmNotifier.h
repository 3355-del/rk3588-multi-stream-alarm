/*
 * TcpAlarmNotifier.h — TCP 告警上报（真实实现，非 mock）
 *
 * 协议：4 字节大端长度头 + JSON 正文（长度头防粘包）。
 * 例：
 *   [0x00 0x00 0x00 0x4A] { "type":"alarm", "stream_id":0, ... }
 *
 * 使用 POSIX / Winsock TCP、非阻塞 socket、限时等待和发送互斥锁，
 * 保证多线程（多路告警线程）并发上报安全。
 */
#ifndef MULTI_STREAM_TCPALARMNOTIFIER_H
#define MULTI_STREAM_TCPALARMNOTIFIER_H

#include <mutex>
#include <chrono>
#include <cstdint>
#include <string>

#include "msad/Hal.h"

namespace msad {

class TcpAlarmNotifier : public IAlarmNotifier {
public:
    ~TcpAlarmNotifier() override;

    bool connect(const std::string& host, int port) override;
    bool sendAlarm(const AlarmInfo& alarm) override;
    void close() override;

public:
    bool sendPayload(const std::string& payload, const std::string& eventId);
    /* 将告警信息序列化为 JSON 字符串（本文件内部实现，便于扩展） */
    static std::string alarmToJson(const AlarmInfo& alarm);

    private:
    bool receiveAck(const std::string& eventId);
    /* 发送一条消息：长度头 + 数据，返回 true=成功 */
    bool sendRaw(const std::string& payload);

    std::mutex mtx_;         /* 保护 socket 发送（多线程并发） */
    std::intptr_t sockFd_ = -1;        /* 连接套接字，-1=未连接 */
    bool connected_ = false;
    bool connectLocked();
    void closeLocked();
    std::string host_;
    int port_ = 0;
    std::chrono::steady_clock::time_point nextRetry_{};
};

} // namespace msad

#endif // MULTI_STREAM_TCPALARMNOTIFIER_H
