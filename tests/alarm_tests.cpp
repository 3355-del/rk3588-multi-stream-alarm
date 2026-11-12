#include "msad/VideoPipeline.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
using namespace msad;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
namespace msad {
// Deterministic queue/dispatch tests without timing-dependent disk throttling.
struct VideoPipelineTestAccess {
    static void prepare(VideoPipeline& p) {
        p.regions_.resize(1);
        const auto& r=p.cfg_.streams[0].regions[0];
        PolygonRegion region(r.id,r.polygon); region.setCooldownMs(r.cooldownMs);
        p.regions_[0].push_back(std::move(region));
        p.stats_.resize(1);
        p.alarmTasks_=std::make_unique<RingBuffer<VideoPipeline::AlarmTask>>(p.cfg_.alarmQueueCapacity);
    }
    static void dispatch(VideoPipeline& p, DetectResult& result, uint64_t tick=0) {
        p.assignTracks(0,result,tick);
        p.checkAndRaiseAlarm(0,result);
    }
    static void failStream(VideoPipeline& p, int idx) { p.stopStream(idx, "test source failure"); }
    static size_t queued(VideoPipeline& p) { return p.alarmTasks_->size(); }
    static size_t rejected(VideoPipeline& p) { return p.alarmTasks_->rejectedCount(); }
    static auto pop(VideoPipeline& p) {
        VideoPipeline::AlarmTask task;
        require(p.alarmTasks_->size()>0 && p.alarmTasks_->pop(task),"expected queued alarm");
        return task;
    }
    static size_t cancel(VideoPipeline& p) {
        p.alarmTasks_->stop(); p.alarmTasks_->stop();
        return p.alarmTasks_->cancelledCount();
    }
};
}
AppConfig configuration(unsigned cooldown,int capacity) {
    AppConfig cfg;cfg.frameWidth=128;cfg.frameHeight=96;cfg.runSeconds=0;
    cfg.alarmQueueCapacity=capacity;cfg.alarm.port=1;cfg.screenshotDir="alarm_test_shots";
    StreamConfig stream;stream.id=10;
    RegionConfig region;region.id=4;region.cooldownMs=cooldown;
    region.polygon={{0,0},{128,0},{128,96},{0,96}};
    stream.regions.push_back(region);cfg.streams.push_back(stream);return cfg;
}
DetectResult frame(std::vector<int> classes) {
    DetectResult result;result.streamId=10;result.frame.streamId=10;
    result.frame.width=128;result.frame.height=96;result.frame.frameIndex=42;
    result.frame.data.assign(result.frame.yuvSize(),128);
    for(int c:classes) result.detections.push_back({{10,10,20,20},c,.9f});
    return result;
}
int main() {
    try {
        {
            auto cfg = configuration(0, 8);
            auto second = cfg.streams.front();
            second.id = 11;
            cfg.streams.push_back(second);
            std::array<std::atomic<int>, 2> observed{};
            VideoPipeline p(cfg, {}, [&](const DetectResult& r) {
                ++observed.at(r.streamId - 10);
            }, true);
            require(p.start(), "two mock streams did not start");
            const auto waitForFrames = [&](int first, int secondCount) {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while ((observed[0] < first || observed[1] < secondCount) &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return observed[0] >= first && observed[1] >= secondCount;
            };
            require(waitForFrames(1, 1), "mock streams did not produce frames");
            VideoPipelineTestAccess::failStream(p, 0);
            VideoPipelineTestAccess::failStream(p, 0);
            require(p.isRunning() && !p.failed(), "single stream fault stopped the pipeline");
            require(waitForFrames(1, observed[1] + 3), "healthy stream stopped producing frames");
            VideoPipelineTestAccess::failStream(p, 1);
            require(!p.isRunning() && p.failed(), "all streams stopped without reporting failure");
            p.stop();
        }
        {
            VideoPipeline p(configuration(5000,8));VideoPipelineTestAccess::prepare(p);
            auto first=frame({0,0});
            first.detections[1].box.x=70;
            VideoPipelineTestAccess::dispatch(p,first);
            require(VideoPipelineTestAccess::queued(p)==2,"two people in one region must generate two alarms");
            const auto left=VideoPipelineTestAccess::pop(p).info.trackId;
            const auto right=VideoPipelineTestAccess::pop(p).info.trackId;
            require(left!=0 && right!=0 && left!=right,"same-class targets shared a track");
            auto repeated=frame({0,0});
            repeated.detections[0].box.x=71;
            repeated.detections[1].box.x=11;
            VideoPipelineTestAccess::dispatch(p,repeated,100);
            require(repeated.detections[0].trackId==right && repeated.detections[1].trackId==left,
                    "track changed when detection order changed");
            require(VideoPipelineTestAccess::queued(p)==0,"repeated people entered cooldown independently");
            auto newcomer=frame({0,0,0});
            newcomer.detections[1].box.x=70;
            newcomer.detections[2].box.x=40;
            VideoPipelineTestAccess::dispatch(p,newcomer,200);
            require(VideoPipelineTestAccess::queued(p)==1,"new person blocked by existing people's cooldown");
            const auto third=VideoPipelineTestAccess::pop(p).info.trackId;
            require(third!=left && third!=right,"new person reused an active track");
            auto missing=frame({});
            VideoPipelineTestAccess::dispatch(p,missing,300);
            auto returned=frame({0});
            VideoPipelineTestAccess::dispatch(p,returned,600);
            require(returned.detections[0].trackId==left && VideoPipelineTestAccess::queued(p)==0,
                    "brief missing detection lost its track or cooldown");
            auto expired=frame({0});
            VideoPipelineTestAccess::dispatch(p,expired,1701);
            require(expired.detections[0].trackId!=left && VideoPipelineTestAccess::queued(p)==1,
                    "expired track blocked a new event");
        }
        {
            auto cfg=configuration(60000,1);
            VideoPipeline p(cfg);VideoPipelineTestAccess::prepare(p);
            auto first=frame({0,1});
            VideoPipelineTestAccess::dispatch(p,first);
            require(VideoPipelineTestAccess::queued(p)==1 && VideoPipelineTestAccess::rejected(p)==1,"queue not bounded/rejection missing");
            auto accepted=VideoPipelineTestAccess::pop(p);
            require(accepted.info.classId==0,"full queue evicted accepted alarm");
            auto second=frame({0,1});
            VideoPipelineTestAccess::dispatch(p,second);
            require(VideoPipelineTestAccess::queued(p)==1,"rejected class entered cooldown or accepted class lost cooldown");
            auto retry=VideoPipelineTestAccess::pop(p);
            require(retry.info.classId==1,"rejected alarm could not retry next frame");
        }
        {
            VideoPipeline p(configuration(0,8));VideoPipelineTestAccess::prepare(p);
            auto result=frame({0,0,0,0,0,0,0,0});
            const auto* pixels=result.frame.data.data();
            VideoPipelineTestAccess::dispatch(p,result);
            require(VideoPipelineTestAccess::queued(p)==8,"zero cooldown suppressed same-class targets");
            std::set<std::string> names;
            std::shared_ptr<const Frame> snapshot;
            for(int i=0;i<8;++i) {
                auto task=VideoPipelineTestAccess::pop(p);
                require(task.frame->data.data()==pixels,"alarm copied pixel buffer");
                if(snapshot) require(snapshot==task.frame,"same-frame alarms do not share snapshot");
                snapshot=task.frame;
                require(task.shotPath.find("_f42_a")!=std::string::npos,"missing frame/sequence in filename");
                require(names.insert(task.shotPath).second,"duplicate screenshot filename");
            }
            auto pending=frame({0,1});
            VideoPipelineTestAccess::dispatch(p,pending);
            require(VideoPipelineTestAccess::cancel(p)==2,"stop cancellations not counted exactly once");
        }
        {
            auto cfg=configuration(0,8);
            VideoPipeline p(cfg,[](const Frame&,const std::string&,const std::vector<Detection>&)->bool {
                throw std::runtime_error("injected screenshot IO failure");
            });
            require(p.start(),"IO failure setup failed");
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(p.isRunning() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(p.failed() && !p.isRunning(),"IO exception did not stop pipeline");
            auto begin=std::chrono::steady_clock::now();p.stop();p.stop();
            require(std::chrono::steady_clock::now()-begin<std::chrono::seconds(2),"IO failure prevented thread cleanup");
        }
        {
            std::string error;
            auto cfg=configuration(0,8);
            require(cfg.validate(error),"default small config rejected");
            cfg.frameWidth=4096;cfg.frameHeight=4096;cfg.queueCapacity=4;
            require(!cfg.validate(error),"PNG/IO memory omitted from budget");
            cfg=configuration(0,65);require(!cfg.validate(error),"alarm queue capacity unchecked");
            cfg=configuration(0,8);const auto before=cfg.estimatedBufferBytes();
            cfg.alarmQueueCapacity=16;require(cfg.estimatedBufferBytes()>before,"alarm capacity missing from estimate");
        }
        std::cout<<"alarm regression tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
