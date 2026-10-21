#include "msad/RkMppSource.h"
#include "msad/RealComponents.h"
#include "msad/PngWriter.h"
#include "msad/Nms.h"
#include <iostream>
#include <cmath>
#include <stdexcept>
using namespace msad;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void compare(const std::vector<Detection>& a,const std::vector<Detection>& b){
    require(a.size()==b.size(),"detection count differs");
    for(size_t i=0;i<a.size();++i){require(a[i].classId==b[i].classId,"class differs");
        require(std::abs(a[i].confidence-b[i].confidence)<.0001f,"confidence differs");
        require(std::abs(a[i].box.x-b[i].box.x)<.05f&&std::abs(a[i].box.y-b[i].box.y)<.05f&&
                std::abs(a[i].box.w-b[i].box.w)<.05f&&std::abs(a[i].box.h-b[i].box.h)<.05f,"box differs");}
}
int main(int argc,char** argv){
    if(argc!=3){std::cerr<<"hardware_tests MODEL VIDEO\n";return 2;}
    try{
        RkMppSource source;require(source.open(argv[2]),"source open");
        RgaFrameProcessor dma(640,640,false,true),cpu(640,640,false,false);
        require(dma.init()&&cpu.init(),"preprocess init");
        RknnInferenceEngine quant(argv[1],.25f);require(quant.init(),"quant init");
        RknnInferenceEngine cpuQuant(argv[1],.25f,quant.sharedModel(),true);require(cpuQuant.init(),"CPU quant init");
        RknnInferenceEngine floats(argv[1],.25f,quant.sharedModel(),false);require(floats.init(),"float init");
        size_t detections=0;
        for(int i=0;i<12;++i){
            Frame decoded,dmaFrame,cpuFrame;require(source.readFrame(decoded),"decode");
            require(decoded.dmaFd>=0&&decoded.data.empty(),"decoder copied CPU pixels");
            require(dma.process(decoded,dmaFrame),"DMA preprocess");
            cpuFrame=dmaFrame; // Deliberately reuse a populated DMA output object.
            require(cpu.process(decoded,cpuFrame),"CPU preprocess");
            require(cpuFrame.dmaFd==-1&&!cpuFrame.bufferOwner&&!cpuFrame.copyRgb&&
                    !cpuFrame.mappedData&&cpuFrame.dmaBytes==0,"stale DMA state in CPU output");
            std::vector<uint8_t> rgb;require(dmaFrame.copyRgb(rgb),"DMA screenshot read");
            require(rgb.size()==cpuFrame.data.size(),"RGB shape");
            double error=0;for(size_t j=0;j<rgb.size();++j)error+=std::abs(int(rgb[j])-int(cpuFrame.data[j]));
            require(error/static_cast<double>(rgb.size())<2,"DMA/CPU preprocess image differs");
            Frame samePixels;samePixels.width=640;samePixels.height=640;samePixels.data=rgb;
            std::vector<Detection> nativeDma,nativeCpu,floatCpu;
            require(quant.infer(dmaFrame,nativeDma),"DMA inference");
            require(cpuQuant.infer(samePixels,nativeCpu),"CPU inference");
            require(floats.infer(samePixels,floatCpu),"float inference");
            compare(nativeDma,nativeCpu);compare(nativeCpu,floatCpu);detections+=nativeDma.size();
            if(i==0)require(PngWriter::saveFrameAsPng(dmaFrame,"hardware_test_frame.png",nms(nativeDma)),"DMA PNG");
        }
        Frame retained;require(source.readFrame(retained),"retain frame");source.close();
        Frame output;require(dma.process(retained,output),"decoded lifetime after source close");
        std::cout<<"PASS: 12 frames DMA/CPU input + INT8/float detection equivalence, detections="<<detections<<"; frame lifetime + PNG\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
