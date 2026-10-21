#pragma once
#include "msad/TcpAlarmNotifier.h"
#include <filesystem>
#include <thread>
#include <atomic>
#include <condition_variable>
namespace msad {
std::string uniqueEventId();
// One owner per directory, durable acceptance followed by asynchronous ACK delivery.
class DurableAlarmNotifier final : public IAlarmNotifier {
public:
 explicit DurableAlarmNotifier(const std::string& directory);
 ~DurableAlarmNotifier() override;
 bool connect(const std::string& host,int port) override;
 bool sendAlarm(const AlarmInfo& alarm) override;
 void close() override;
private:
 void run();
 std::filesystem::path directory_;
 std::intptr_t lock_=-1;
 TcpAlarmNotifier tcp_;
 std::mutex mutex_;
 std::condition_variable wake_;
 std::thread worker_;
 bool stop_=false;
 std::string error_;
};
}
