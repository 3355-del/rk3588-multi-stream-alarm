#include "msad/DurableAlarmNotifier.h"
#include "msad/Json.h"
#include "msad/Logger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace msad {
std::string uniqueEventId() {
    static std::mutex m; static std::random_device rd;
    std::lock_guard<std::mutex> guard(m);
    std::ostringstream out;
    for(int i=0;i<4;++i) out<<std::hex<<std::setw(8)<<std::setfill('0')<<static_cast<uint32_t>(rd());
    return out.str();
}
static void syncFile(const std::filesystem::path& p) {
#ifdef _WIN32
    int fd=_wopen(p.c_str(),_O_RDWR|_O_BINARY);
    if(fd<0) throw std::runtime_error("cannot open outbox file for sync");
    int result=_commit(fd); _close(fd);
#else
    int fd=::open(p.c_str(),O_RDONLY);
    if(fd<0) throw std::runtime_error("cannot open outbox file for sync");
    int result=fsync(fd); ::close(fd);
#endif
    if(result!=0) throw std::runtime_error("outbox sync failed");
}
static void syncDirectory(const std::filesystem::path& p) {
#ifndef _WIN32
    int fd=::open(p.c_str(),O_RDONLY|O_DIRECTORY);
    if(fd<0) throw std::runtime_error("cannot sync outbox directory");
    int r=fsync(fd); ::close(fd); if(r) throw std::runtime_error("directory sync failed");
#else
    (void)p; // File content flushed; sudden power-loss rename durability depends on filesystem.
#endif
}
DurableAlarmNotifier::DurableAlarmNotifier(const std::string& directory):directory_(directory) {
    std::filesystem::create_directories(directory_);
    auto p=directory_/"owner.lock";
#ifdef _WIN32
    HANDLE h=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("outbox already in use or inaccessible");
    lock_=reinterpret_cast<std::intptr_t>(h);
#else
    int fd=::open(p.c_str(),O_CREAT|O_RDWR,0600);
    if(fd<0) throw std::runtime_error("cannot lock outbox");
    if(flock(fd,LOCK_EX|LOCK_NB)) { ::close(fd); throw std::runtime_error("outbox already in use"); }
    lock_=fd;
#endif
}
DurableAlarmNotifier::~DurableAlarmNotifier() { close(); }
bool DurableAlarmNotifier::connect(const std::string& host,int port) {
    // Bind stored events to a destination; never silently replay them to a different server.
    auto destination=directory_/"destination.txt";
    std::string expected=host+":"+std::to_string(port);
    if(std::filesystem::exists(destination)) {
        std::ifstream in(destination); std::string previous; std::getline(in,previous);
        if(previous!=expected) throw std::runtime_error("outbox destination changed: use a different outbox_dir");
    } else { std::ofstream out(destination); out<<expected; out.close(); if(!out) throw std::runtime_error("cannot save outbox destination"); syncFile(destination); }
    bool connected=tcp_.connect(host,port);
    if(!worker_.joinable()) worker_=std::thread(&DurableAlarmNotifier::run,this);
    return connected;
}
bool DurableAlarmNotifier::sendAlarm(const AlarmInfo& input) {
    std::lock_guard<std::mutex> guard(mutex_);
    if(stop_) throw std::runtime_error("outbox is closed");
    if(!error_.empty()) throw std::runtime_error(error_);
    size_t count=0;
    for(const auto& f:std::filesystem::directory_iterator(directory_)) if(f.path().extension()==".json") ++count;
    if(count>=10000) throw std::runtime_error("outbox limit 10000 reached; pending events preserved");
    AlarmInfo alarm=input; if(alarm.eventId.empty()) alarm.eventId=uniqueEventId();
    if(alarm.eventId.find_first_not_of("0123456789abcdef-")!=std::string::npos) throw std::runtime_error("invalid event ID");
    auto target=directory_/(alarm.eventId+".json");
    if(std::filesystem::exists(target)) throw std::runtime_error("duplicate event ID in outbox");
    auto temporary=directory_/(alarm.eventId+".tmp");
    std::string payload=TcpAlarmNotifier::alarmToJson(alarm);
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc); out<<payload;out.close();
    if(!out) throw std::runtime_error("cannot write durable alarm");
    syncFile(temporary);std::filesystem::rename(temporary,target);syncDirectory(directory_);
    wake_.notify_one();return true;
}
void DurableAlarmNotifier::run() {
    try {
        while(true) {
            std::vector<std::filesystem::path> paths;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                if(stop_) break;
                for(const auto& f:std::filesystem::directory_iterator(directory_)) if(f.path().extension()==".json") paths.push_back(f.path());
            }
            std::sort(paths.begin(),paths.end());
            for(const auto& p:paths) {
                { std::lock_guard<std::mutex> guard(mutex_); if(stop_) return; }
                if(std::filesystem::file_size(p)>1024*1024) throw std::runtime_error("oversized outbox record");
                std::ifstream in(p,std::ios::binary);std::string payload((std::istreambuf_iterator<char>(in)),{});
                in.close();
                Json record;std::string error;
                if(!Json::parse(payload,record,error) || record["event_id"].asString()!=p.stem().string()) throw std::runtime_error("invalid outbox record; preserved for inspection");
                if(!tcp_.sendPayload(payload,p.stem().string())) break;
                { std::lock_guard<std::mutex> guard(mutex_); std::filesystem::remove(p);syncDirectory(directory_); }
                Logger::info("DurableAlarm","acked="+p.stem().string());
            }
            std::unique_lock<std::mutex> guard(mutex_);
            wake_.wait_for(guard,std::chrono::seconds(1),[this]{return stop_;});
        }
    } catch(const std::exception& e) {
        std::lock_guard<std::mutex> guard(mutex_);error_=e.what();Logger::error("DurableAlarm",error_);
    }
}
void DurableAlarmNotifier::close() {
    { std::lock_guard<std::mutex> guard(mutex_);stop_=true; }
    wake_.notify_all();if(worker_.joinable()) worker_.join();tcp_.close();
    if(lock_!=-1) {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(lock_));
#else
        ::close(static_cast<int>(lock_));
#endif
        lock_=-1;
    }
}
}
