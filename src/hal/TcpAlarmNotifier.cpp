#include "msad/TcpAlarmNotifier.h"
#include "msad/Json.h"
#include "msad/Logger.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace msad {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
int socketError() { return WSAGetLastError(); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
bool interrupted(int e) { return e == WSAEINTR; }
void closeSocket(Socket s) { closesocket(s); }
bool nonblocking(Socket s) { u_long mode = 1; return ioctlsocket(s, FIONBIO, &mode) == 0; }
bool initializeSockets() {
    struct Runtime {
        bool ok; Runtime() { WSADATA data{}; ok = WSAStartup(MAKEWORD(2,2), &data) == 0; }
        ~Runtime() { if (ok) WSACleanup(); }
    };
    static Runtime runtime;
    return runtime.ok;
}
#else
using Socket = int;
constexpr Socket invalidSocket = -1;
int socketError() { return errno; }
bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS; }
bool interrupted(int e) { return e == EINTR; }
void closeSocket(Socket s) { ::close(s); }
bool nonblocking(Socket s) { int flags = fcntl(s, F_GETFL, 0); return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0; }
bool initializeSockets() { return true; }
#endif
using Clock = std::chrono::steady_clock;
bool writable(Socket s, Clock::time_point deadline) {
#ifndef _WIN32
    if (s >= FD_SETSIZE) return false;
#endif
    while (Clock::now() < deadline) {
        fd_set writeSet, errorSet;
        FD_ZERO(&writeSet); FD_ZERO(&errorSet); FD_SET(s, &writeSet); FD_SET(s, &errorSet);
        timeval timeout{}; timeout.tv_usec = 50000;
#ifdef _WIN32
        int n = select(0, nullptr, &writeSet, &errorSet, &timeout);
#else
        int n = select(s + 1, nullptr, &writeSet, &errorSet, &timeout);
#endif
        if (n > 0) return !FD_ISSET(s, &errorSet) && FD_ISSET(s, &writeSet);
        if (n < 0 && !interrupted(socketError())) return false;
    }
    return false;
}
}

TcpAlarmNotifier::~TcpAlarmNotifier() { close(); }
void TcpAlarmNotifier::closeLocked() {
    if (sockFd_ != -1) closeSocket(static_cast<Socket>(sockFd_));
    sockFd_ = -1; connected_ = false;
}
bool TcpAlarmNotifier::connect(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (host != host_ || port != port_) { closeLocked(); nextRetry_ = {}; }
    host_ = host; port_ = port;
    return connectLocked();
}
bool TcpAlarmNotifier::connectLocked() {
    if (connected_) return true;
    if (Clock::now() < nextRetry_) return false;
    nextRetry_ = Clock::now() + std::chrono::seconds(1);
    if (port_ < 1 || port_ > 65535 || !initializeSockets()) return false;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<uint16_t>(port_));
    // Numeric IPv4 avoids an unbounded DNS call on the alarm path.
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) return false;
    Socket s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == invalidSocket) return false;
    if (!nonblocking(s)) { closeSocket(s); return false; }
    int result = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result != 0) {
        int e = socketError();
        if ((!wouldBlock(e) && !interrupted(e)) || !writable(s, Clock::now() + std::chrono::milliseconds(500))) {
            closeSocket(s); return false;
        }
        int error = 0;
#ifdef _WIN32
        int size = sizeof(error);
        result = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
#else
        socklen_t size = sizeof(error);
        result = getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &size);
#endif
        if (result != 0 || error != 0) { closeSocket(s); return false; }
    }
    sockFd_ = static_cast<std::intptr_t>(s); connected_ = true;
    Logger::info("TcpAlarm", "connected to " + host_ + ":" + std::to_string(port_));
    return true;
}
bool TcpAlarmNotifier::sendRaw(const std::string& payload) {
    if (!connected_ || payload.size() > std::numeric_limits<uint32_t>::max()) return false;
    const auto len = static_cast<uint32_t>(payload.size());
    std::vector<char> packet = {static_cast<char>(len >> 24), static_cast<char>(len >> 16),
                                static_cast<char>(len >> 8), static_cast<char>(len)};
    packet.insert(packet.end(), payload.begin(), payload.end());
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    size_t offset = 0; Socket s = static_cast<Socket>(sockFd_);
    while (offset < packet.size() && Clock::now() < deadline) {
        const int chunk = static_cast<int>(std::min<size_t>(packet.size() - offset, 65536));
#ifdef _WIN32
        int n = ::send(s, packet.data() + offset, chunk, 0);
#else
        auto n = ::send(s, packet.data() + offset, static_cast<size_t>(chunk), MSG_NOSIGNAL);
#endif
        if (n > 0) { offset += static_cast<size_t>(n); continue; }
        if (n == 0) return false;
        int e = socketError();
        if (interrupted(e)) continue;
        if (!wouldBlock(e) || !writable(s, deadline)) return false;
    }
    return offset == packet.size();
}
bool TcpAlarmNotifier::sendAlarm(const AlarmInfo& alarm) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!connectLocked()) return false;
    if (sendRaw(alarmToJson(alarm))) return true;
    // A short packet must never be followed by a new header on this connection.
    closeLocked(); nextRetry_ = Clock::now() + std::chrono::seconds(1);
    Logger::warn("TcpAlarm", "send failed; connection closed, next alarm will retry");
    return false;
}
bool TcpAlarmNotifier::receiveAck(const std::string& eventId) {
    const auto deadline=Clock::now()+std::chrono::milliseconds(1000);
    Socket s=static_cast<Socket>(sockFd_);
#ifndef _WIN32
    if (s>=FD_SETSIZE) return false;
#endif
    auto exact=[&](char* dst, size_t count) {
        size_t pos=0;
        while(pos<count && Clock::now()<deadline) {
            fd_set reads; FD_ZERO(&reads); FD_SET(s,&reads);
            timeval tv{}; tv.tv_usec=50000;
#ifdef _WIN32
            int ready=select(0,&reads,nullptr,nullptr,&tv);
#else
            int ready=select(s+1,&reads,nullptr,nullptr,&tv);
#endif
            if(ready<0) { if(interrupted(socketError())) continue; return false; }
            if(!ready) continue;
            int got=static_cast<int>(::recv(s,dst+pos,static_cast<int>(count-pos),0));
            if(got>0) pos+=static_cast<size_t>(got);
            else if(got==0) return false;
            else if(!wouldBlock(socketError()) && !interrupted(socketError())) return false;
        }
        return pos==count;
    };
    unsigned char h[4]; if(!exact(reinterpret_cast<char*>(h),4)) return false;
    uint32_t len=(uint32_t(h[0])<<24)|(uint32_t(h[1])<<16)|(uint32_t(h[2])<<8)|h[3];
    if(!len || len>4096) return false;
    std::string body(len,'\0'); if(!exact(body.data(),len)) return false;
    Json ack; std::string error;
    return Json::parse(body,ack,error) && ack["type"].asString()=="ack" && ack["event_id"].asString()==eventId;
}
bool TcpAlarmNotifier::sendPayload(const std::string& payload,const std::string& eventId) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(!connectLocked()) return false;
    if(sendRaw(payload) && receiveAck(eventId)) return true;
    closeLocked(); nextRetry_=Clock::now()+std::chrono::seconds(1); return false;
}
void TcpAlarmNotifier::close() {
    std::lock_guard<std::mutex> lock(mtx_);
    closeLocked(); host_.clear(); port_ = 0; nextRetry_ = {};
}
std::string TcpAlarmNotifier::alarmToJson(const AlarmInfo& a) {
    Json root(Json::Type::Object);
    if (!a.eventId.empty()) root["event_id"] = a.eventId;
    root["type"] = "alarm"; root["timestamp_ms"] = static_cast<double>(a.timestampMs);
    root["stream_id"] = a.streamId; root["region_id"] = a.regionId;
    root["class_id"] = a.classId; root["class_name"] = std::string(cocoClassName(a.classId));
    root["confidence"] = static_cast<double>(a.confidence);
    root["center_x"] = static_cast<double>(a.centerX); root["center_y"] = static_cast<double>(a.centerY);
    root["screenshot"] = a.screenshotFile;
    return root.dump();
}
} // namespace msad
