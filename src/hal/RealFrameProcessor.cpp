/*
 * RealFrameProcessor.cpp — RGA 硬件图像预处理（IFrameProcessor 的 RK3588 实现）
 *
 * 输入：CPU YUYV422/RGB888，或携带 fd、stride、owner 的 NV12 DMA 帧
 * 输出：outWidth x outHeight RGB888（默认 640x640，即 YOLO 模型输入尺寸）
 *
 * 为什么用 letterbox（等比缩放 + 居中黑边）而不是直接拉伸：
 *   640x480 直接拉伸到 640x640 会让目标纵向拉长 33%，检测框随之变形；
 *   等比缩放保证目标几何形状不变，检测框与人工画的警戒区在坐标上完全一致。
 *
 * 默认 DMA 分支由 RGA 完成缩放、格式转换和居中填充；不逐帧搬回 CPU。
 * dmaInput=false 的对照分支才用 CPU 拼接黑边。截图和首次调试导出按需读取 DMA。
 */
#include "msad/RealComponents.h"
#include "msad/DmaBuffer.h"
#include "msad/FrameDump.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>

#include <im2d.h>
#include <RgaUtils.h>

#include "msad/Logger.h"

namespace msad {

namespace {

/* 取偶数（RGA 对非偶宽高支持不佳），并保证 >= 2 */
int evenFloor(int v) {
    return std::max(2, v & ~1);
}

/*
 * 调试辅助：设置环境变量 MSAD_DUMP_FRAME=/path/frame.ppm 时，
 * 把预处理后的第一帧（含 letterbox 黑边的模型输入图）存成 PPM，
 * 便于确认"模型看到的图"是否正确（检测不到目标时先查这张图）。
 */
void dumpFrameOnce(const Frame& frame) {
    const char* path=std::getenv("MSAD_DUMP_FRAME");
    if(!path||!*path)return;
    static FrameDump dump;
    if(!dump.saveOnce(frame,path))Logger::warn("RGA","debug frame export failed");
}

} // namespace

RgaFrameProcessor::RgaFrameProcessor(int outWidth, int outHeight, bool flip180, bool dmaInput)
    : outW_(outWidth), outH_(outHeight), flip180_(flip180), dmaInput_(dmaInput) {}

RgaFrameProcessor::~RgaFrameProcessor() = default;

bool RgaFrameProcessor::init() {
    if (outW_ < 2 || outH_ < 2 || outW_ > 4096 || outH_ > 4096) {
        Logger::error("RGA", "invalid output size " + std::to_string(outW_) + "x" + std::to_string(outH_));
        return false;
    }
    Logger::info("RGA", "frame processor ready, output=" + std::to_string(outW_) + "x" +
                            std::to_string(outH_) + " RGB888 (letterbox)" +
                            (flip180_ ? ", rotate 180" : ""));
    return true;
}

bool RgaFrameProcessor::process(Frame in, Frame& out) {
    const bool decodedDma=in.pixelFormat==Frame::PixelFormat::NV12&&in.dmaFd>=0&&in.bufferOwner;
    if (!in.validDimensions() || (in.data.empty()&&!decodedDma)) {
        Logger::error("RGA", "invalid input frame");
        return false;
    }
    const int srcW = in.width;
    const int srcH = in.height;
    const size_t pixels = static_cast<size_t>(srcW) * static_cast<size_t>(srcH);

    /* 1. 识别输入格式：DMA 使用显式格式，旧 CPU 帧兼容按字节数识别 */
    int rgaFormat = 0;
    if(decodedDma) {
        if(in.widthStride<in.width||in.heightStride<in.height)return false;
        rgaFormat=RK_FORMAT_YCbCr_420_SP;
    } else if (in.data.size() == pixels * 2) {
        rgaFormat = RK_FORMAT_YUYV_422;
    } else if (in.data.size() == pixels * 3) {
        rgaFormat = RK_FORMAT_RGB_888;
    } else {
        Logger::error("RGA", "unsupported input format, data size=" + std::to_string(in.data.size()) +
                                 " (expect YUYV422 or RGB888)");
        return false;
    }

    /* 2. 计算等比缩放后的有效图像尺寸与居中偏移 */
    const double scale = std::min(static_cast<double>(outW_) / srcW,
                                  static_cast<double>(outH_) / srcH);
    const int fitW = evenFloor(static_cast<int>(srcW * scale + 0.5));
    const int fitH = evenFloor(static_cast<int>(srcH * scale + 0.5));
    const int offX = (outW_ - fitW) / 2;
    const int offY = (outH_ - fitH) / 2;

    if(dmaInput_){
        // RGA writes the whole letterboxed RGB image into an imported DMA buffer.
        const int stride=(outW_+15)&~15;
        auto buffer=DmaBuffer::allocate(static_cast<size_t>(stride)*outH_*3);
        if(!buffer){Logger::error("RGA","DMA heap allocation failed");return false;}
        auto src=decodedDma?wrapbuffer_fd(in.dmaFd,srcW,srcH,rgaFormat,in.widthStride,in.heightStride)
            :wrapbuffer_virtualaddr(in.data.data(),srcW,srcH,rgaFormat);
        auto dst=wrapbuffer_fd(buffer->fd(),outW_,outH_,RK_FORMAT_RGB_888,stride,outH_);
        im_rect whole{0,0,outW_,outH_};
        auto result=imfill(dst,whole,0xff000000);
        if(result==IM_STATUS_SUCCESS){
            im_rect sourceRect{0,0,srcW,srcH}, targetRect{offX,offY,fitW,fitH};
            result=improcess(src,dst,{},sourceRect,targetRect,{},IM_SYNC|(flip180_?IM_HAL_TRANSFORM_ROT_180:0));
        }
        if(result!=IM_STATUS_SUCCESS){Logger::error("RGA",std::string("DMA letterbox failed: ")+imStrError(result));return false;}
        out=Frame{};out.width=outW_;out.height=outH_;out.streamId=in.streamId;
        out.timestampMs=in.timestampMs;out.frameIndex=in.frameIndex;out.pixelFormat=Frame::PixelFormat::RGB888;
        out.dmaFd=buffer->fd();out.widthStride=stride;out.heightStride=outH_;out.dmaBytes=buffer->size();
        out.mappedData=buffer->data();out.bufferOwner=buffer;
        out.copyRgb=[buffer,w=outW_,h=outH_,stride](std::vector<uint8_t>& rgb){return buffer->copyRgb(w,h,stride,rgb);};
        dumpFrameOnce(out);
        return true;
    }

    if (fit_.size() != static_cast<size_t>(fitW) * fitH * 3) {
        fit_.assign(static_cast<size_t>(fitW) * fitH * 3, 0);
    }

    /* 3. RGA 一次完成缩放 + YUYV→RGB 转换（硬件加速） */
    rga_buffer_t src = decodedDma ? wrapbuffer_fd(in.dmaFd,srcW,srcH,rgaFormat,in.widthStride,in.heightStride)
        : wrapbuffer_virtualaddr(in.data.data(), srcW, srcH, rgaFormat);
    rga_buffer_t dst = wrapbuffer_virtualaddr(fit_.data(), fitW, fitH, RK_FORMAT_RGB_888);
    const IM_STATUS st = imresize(src, dst);
    if (st != IM_STATUS_SUCCESS) {
        Logger::error("RGA", std::string("imresize failed: ") + imStrError(st));
        return false;
    }

    /* 4. 居中放入输出缓冲（黑边填充，保证 letterbox） */
    out=Frame{};
    out.pixelFormat=Frame::PixelFormat::RGB888;
    out.data.assign(static_cast<size_t>(outW_) * outH_ * 3, 0);
    uint8_t* dstBase = out.data.data();
    const size_t rowBytes = static_cast<size_t>(fitW) * 3;
    for (int y = 0; y < fitH; ++y) {
        const int dstRow = flip180_ ? (offY + fitH - 1 - y) : (offY + y);
        uint8_t* outRow = dstBase + (static_cast<size_t>(dstRow) * outW_ + offX) * 3;
        const uint8_t* inRow = fit_.data() + static_cast<size_t>(y) * rowBytes;
        if (!flip180_) {
            std::memcpy(outRow, inRow, rowBytes);
        } else {
            /* 行内像素逆序 + 行序颠倒 = 整幅 180° 旋转 */
            for (int x = 0; x < fitW; ++x) {
                const uint8_t* p = inRow + static_cast<size_t>(fitW - 1 - x) * 3;
                outRow[static_cast<size_t>(x) * 3 + 0] = p[0];
                outRow[static_cast<size_t>(x) * 3 + 1] = p[1];
                outRow[static_cast<size_t>(x) * 3 + 2] = p[2];
            }
        }
    }

    out.width = outW_;
    out.height = outH_;
    out.streamId = in.streamId;
    out.timestampMs = in.timestampMs;
    out.frameIndex = in.frameIndex;
    dumpFrameOnce(out);
    return true;
}

} // namespace msad
