#pragma once
#include "msad/Types.h"
#include <cstdio>
#include <memory>
#include <mutex>
namespace msad {
// The completed check precedes DMA CPU access, including across concurrent streams.
class FrameDump {
public:
    bool saveOnce(const Frame& frame, const char* path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(done_)return true;
        if(!path||!*path||!frame.validDimensions())return false;
        std::unique_ptr<FILE,decltype(&std::fclose)> file(std::fopen(path,"wb"),&std::fclose);
        if(!file)return false;
        std::vector<uint8_t> copied;
        const auto* rgb=&frame.data;
        if(frame.copyRgb){if(!frame.copyRgb(copied))return false;rgb=&copied;}
        const size_t bytes=static_cast<size_t>(frame.width)*static_cast<size_t>(frame.height)*3;
        if(rgb->size()!=bytes)return false;
        bool ok=std::fprintf(file.get(),"P6\n%d %d\n255\n",frame.width,frame.height)>0;
        ok=std::fwrite(rgb->data(),1,bytes,file.get())==bytes&&ok;
        const int closed=std::fclose(file.release());
        done_=ok&&closed==0;
        return done_;
    }
private:
    std::mutex mutex_;
    bool done_=false;
};
}
