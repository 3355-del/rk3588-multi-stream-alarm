#include "msad/ReconnectingSource.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace msad;
void require(bool ok){if(!ok)throw std::runtime_error("reconnect assertion failed");}
struct Fake : IVideoSource {
    std::atomic<int> opens{0},reads{0};
    int recoverAt=3;
    std::atomic<bool> stopped{false};
    bool open(const std::string&) override {++opens;return true;}
    bool readFrame(Frame& f) override {++reads;if(opens>=recoverAt){f.frameIndex=42;return true;}return false;}
    void close() override {}
    void requestStop() override {stopped=true;}
    std::string name() const override {return "fake";}
};
int main(){
    {
        auto fake=std::make_unique<Fake>();auto* p=fake.get();
        ReconnectingSource source(std::move(fake),5,1);Frame frame;
        require(source.open("rtsp://fixture"));require(source.readFrame(frame));
        require(p->opens==3 && frame.frameIndex==42);
        require(source.readFrame(frame));require(p->opens==3);
    }
    {
        auto fake=std::make_unique<Fake>();auto* p=fake.get();p->recoverAt=99;
        ReconnectingSource source(std::move(fake),2,1);Frame frame;
        require(source.open("rtsp://fixture"));require(!source.readFrame(frame));require(p->opens==3);
    }
    {
        auto fake=std::make_unique<Fake>();auto* p=fake.get();p->recoverAt=99;
        ReconnectingSource source(std::move(fake),5,2000);Frame frame;source.open("rtsp://fixture");
        auto result=std::async(std::launch::async,[&]{return source.readFrame(frame);});
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(p->reads==0 && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        source.requestStop();
        require(result.wait_for(std::chrono::milliseconds(500))==std::future_status::ready);
        require(!result.get() && p->stopped);
    }
    std::cout<<"PASS reconnect recovery, exhaustion, interruptible backoff\n";
}
