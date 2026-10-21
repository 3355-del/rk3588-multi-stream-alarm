#include "msad/FrameDump.h"
#include <atomic>
#include "msad/Config.h"
#include "msad/Json.h"
#include "msad/MockVideoSource.h"
#include "msad/MockInferenceEngine.h"
#include "msad/MockFrameProcessor.h"
#include "msad/Nms.h"
#include "msad/PolygonRegion.h"
#include "msad/PngWriter.h"
#include "msad/RingBuffer.h"
#include "msad/VideoPipeline.h"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace msad;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        std::string error;
        Json j;
        for (const char* bad : {"+1","01","1.","1e","1e999","[1,]","\"a\nb\""}) {
            check(!Json::parse(bad,j,error), "invalid JSON accepted");
        }
        check(Json::parse("{\"a\":[1,true,\"中文\"]}",j,error), "valid JSON rejected");
        check(error.empty(), "stale JSON error");
        check(Json::parse(R"("\uD83D\uDE00")",j,error) && j.asString().size()==4,"surrogate pair");
        check(!Json::parse(R"("\uD800")",j,error),"unpaired surrogate accepted");
        check(!Json::parse(std::string(140,'[')+"0"+std::string(140,']'),j,error),"excess nesting accepted");
        const std::string good = R"({"frame_width":128,"frame_height":96,"fps":20,"run_seconds":0,
            "streams":[{"id":10,"regions":[{"id":4,"polygon":[[0,0],[128,0],[128,96],[0,96]]}]}]})";
        { std::ofstream f("config.json"); f << good; }
        AppConfig cfg;
        check(AppConfig::load("config.json",cfg,error), "valid config rejected");
        check(AppConfig::load("config.json",cfg,error) && cfg.streams.size()==1, "load duplicated streams");
        for (const char* extra : {"\"frame_width\":127", "\"frame_height\":-2", "\"queue_capacity\":-1",
            "\"inference_concurrency\":0", "\"inference_concurrency\":3", "\"inference_concurrency\":1.5", "\"native_outputs\":1", "\"dma_input\":0",
            "\"alarm_queue_capacity\":0", "\"alarm_queue_capacity\":65", "\"alarm_queue_capacity\":1.5", "\"fps\":0", "\"fps\":1.5", "\"run_seconds\":-1", "\"frame_width\":1e30",
            "\"alarm\":{\"port\":70000}", "\"streams\":[{\"id\":1},{\"id\":1}]",
            "\"streams\":[{\"regions\":[{\"cooldown_ms\":-1,\"polygon\":[[0,0],[1,0],[0,1]]}]}]"}) {
            std::string modified = good; modified.pop_back(); modified += ","; modified += extra; modified += "}";
            { std::ofstream f("bad.json"); f << modified; }
            check(!AppConfig::load("bad.json",cfg,error), "bad config accepted");
            check(cfg.streams.size()==1 && cfg.streams[0].id==10 && cfg.frameWidth==128, "failed load changed state");
        }
        for (bool hardware : {false,true}) {
            for (int category : {4,5,79,80}) {
                std::ofstream f("category_config.json");
                f << "{\"hardware\":" << (hardware?"true":"false")
                  << R"(,"model_path":"model.rknn","streams":[{"source_uri":"/dev/video0","regions":[{"polygon":[[0,0],[100,0],[0,100]],"classes":[)"
                  << category << "]}]}]}";
                f.close();
                AppConfig parsed;
                check(AppConfig::load("category_config.json",parsed,error)==(category<(hardware?80:5)),"class range disagrees with hardware mode");
            }
        }
        RingBuffer<int> queue(2);
        queue.push(1); queue.push(2); queue.push(3);
        int v=0; check(queue.droppedCount()==1 && queue.pop(v) && v==2, "queue drop-oldest broken");
        check(queue.pop(v) && v==3, "queue order broken");
        auto waiter=std::async(std::launch::async,[&]{int value;return queue.pop(value);});
        queue.stop();
        check(waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready && !waiter.get(), "queue stop blocked");
        auto scene=std::make_shared<SimScene>(128,96,3);
        MockVideoSource source(scene,20); check(source.open("mock://test"),"source open");
        MockInferenceEngine infer(scene); infer.init();
        Frame first; source.readFrame(first);
        check(first.timestampMs==50 && first.frameIndex==1, "fps timestamp incorrect");
        const auto truth=first.mockDetections;
        Frame later;
        for(int i=0;i<30;++i) source.readFrame(later);
        std::vector<Detection> detections;
        check(infer.infer(first,detections) && detections.size()==truth.size(), "old frame inference failed");
        for(size_t i=0;i<truth.size();++i) check(detections[i].box.x==truth[i].box.x && detections[i].box.y==truth[i].box.y,"frame/result mismatch");
        MockFrameProcessor processor; Frame output; auto* pixels=first.data.data();
        check(processor.process(std::move(first),output) && output.data.data()==pixels,"preprocessing copied pixels");
        auto boxes=nms({{{0,0,10,10},0,.9f},{{0,0,10,10},0,.8f},{{0,0,10,10},1,.85f}});
        check(boxes.size()==2,"class-aware NMS broken");
        check(computeIoU({0,0,10,10},{20,20,10,10})==0,"disjoint IoU");
        PolygonRegion region(0,{{0,0},{100,0},{100,100},{0,100}});
        check(region.contains({50,50}) && region.contains({100,50}) && !region.contains({101,50}),"polygon boundary");
        for (const auto& size : std::vector<std::pair<int,int>>{{2,2},{128,96},{1280,720}}) {
            Frame f; f.width=size.first; f.height=size.second; f.data.assign(f.yuvSize(),128);
            check(PngWriter::saveFrameAsPng(f,"image_"+std::to_string(f.width)+".png",{}),"PNG save failed");
        }
        Frame sharedRgb; sharedRgb.width=2;sharedRgb.height=2;
        sharedRgb.copyRgb=[](std::vector<uint8_t>& out){out.assign(12,127);return true;};
        check(PngWriter::saveFrameAsPng(sharedRgb,"shared_rgb.png",{}),"external RGB snapshot failed");
        sharedRgb.copyRgb=[](std::vector<uint8_t>&){return false;};
        check(!PngWriter::saveFrameAsPng(sharedRgb,"invalid_shared.png",{}),"failed external read accepted");
        FrameDump dumper;std::atomic<int> cpuReads{0};
        Frame debug;debug.width=2;debug.height=2;
        debug.copyRgb=[&](std::vector<uint8_t>& out){++cpuReads;out.assign(12,128);return true;};
        auto dumpA=std::async(std::launch::async,[&]{return dumper.saveOnce(debug,"debug_once.ppm");});
        auto dumpB=std::async(std::launch::async,[&]{return dumper.saveOnce(debug,"debug_once.ppm");});
        check(dumpA.get()&&dumpB.get(),"debug dump failed");
        for(int i=0;i<20;++i)check(dumper.saveOnce(debug,"debug_once.ppm"),"repeat dump failed");
        check(cpuReads==1,"debug dump copies DMA more than once");
        Frame invalid; invalid.width=127; invalid.height=96; invalid.data.resize(20000);
        check(!PngWriter::saveFrameAsPng(invalid,"invalid.png",{}),"odd PNG dimensions accepted");
        Frame f; f.width=128;f.height=96;f.data.assign(f.yuvSize(),128);
        check(PngWriter::saveFrameAsPng(f,"outside.png",{{{0,1000,20,20},0,.9f},{{-1e30f,-1e30f,1,1},0,.9f}}),"outside box save");
        cfg.streams[0].regions.clear(); cfg.screenshotDir="shots"; cfg.alarm.port=1;
        VideoPipeline pipeline(cfg);
        for (int i=0;i<2;++i) {
            check(pipeline.start(),"pipeline start/restart");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            auto before=std::chrono::steady_clock::now(); pipeline.stop(); pipeline.stop();
            check(std::chrono::steady_clock::now()-before < std::chrono::seconds(2),"pipeline stop too slow");
        }
        std::cout << "core regression tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
