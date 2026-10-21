/*
 * RealInferenceEngine.cpp — RKNN NPU 推理（IInferenceEngine 的 RK3588 实现）
 *
 * 模型：YOLOv8n / YOLOv11n INT8（.rknn），COCO 80 类，输入 1x640x640x3 NHWC UINT8。
 *
 * 输出布局（RKNN 优化后的"分离输出"，共 9 个张量，3 个尺度 × 3 组）：
 *   [1, 64, H, W]  框回归 DFL 分布（4 个方向 × 16 个 bin）
 *   [1, 80, H, W]  类别分数
 *   [1,  1, H, W]  置信度（本模型恒为 1，忽略）
 *   尺度 H = 80 / 40 / 20，对应 stride = 8 / 16 / 32。
 *
 * 后处理（对应 Python 参考实现的 dfl / box_process / post_process）：
 *   1) 每个网格位置取最大类别分数；低于阈值直接丢弃；
 *   2) 4 个方向各做 softmax(16) 求期望，得到 ltrb 距离（DFL 解码）；
 *   3) 换算成绝对坐标 (x1,y1,x2,y2) 并裁剪到图像范围；
 *   4) 按置信度降序截断到 kMaxCandidates，交由流水线的 msad::nms 做类别感知 NMS。
 *
 * 说明：这里只做"单帧前向 + 解码"，不做 NMS，保证 NMS 实现全工程单一来源。
 */
#include "msad/RealComponents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <memory>
#include <stdexcept>

#include "rknn_api.h"
#include "npu_session_lease.h"

#include "msad/Logger.h"

namespace msad {

namespace {

constexpr int kDflBins = 16;          /* DFL 每个方向的 bin 数（64 / 4） */
constexpr int kMaxCandidates = 1024;  /* 送入 NMS 前的候选框上限（防 O(n^2) 抖动） */

} // namespace

struct RknnModelContext {
    NpuSessionLease lease;
    rknn_context root = 0;
    std::string path;
    ~RknnModelContext() { if(root) rknn_destroy(root); }
};

RknnInferenceEngine::RknnInferenceEngine(const std::string& modelPath, float confThresh,
    std::shared_ptr<RknnModelContext> sharedModel, bool nativeOutputs)
    : sharedModel_(sharedModel ? std::move(sharedModel) : std::make_shared<RknnModelContext>()),
      nativeOutputs_(nativeOutputs), modelPath_(modelPath), confThresh_(confThresh) {}

RknnInferenceEngine::~RknnInferenceEngine() {
    if(ctx_) rknn_destroy(static_cast<rknn_context>(reinterpret_cast<uintptr_t>(ctx_)));
}

bool RknnInferenceEngine::init() {
    if(ctx_) return false;
    scales_.clear(); outputMeta_.clear();
    if(!std::isfinite(confThresh_) || confThresh_<=0 || confThresh_>1) return false;
    // Initialized serially before workers start. Shared root stays alive until every
    // duplicated engine is destroyed; weights load once per pipeline, not per stream.
    if(!sharedModel_->root) {
        try { sharedModel_->lease.acquire(false); }
        catch(const std::exception& error) { Logger::error("RKNN",error.what()); return false; }
        FILE* fp=std::fopen(modelPath_.c_str(), "rb");
        if(!fp) { Logger::error("RKNN", "cannot open model: "+modelPath_); return false; }
        std::unique_ptr<FILE, decltype(&std::fclose)> file(fp, &std::fclose);
        if(std::fseek(fp,0,SEEK_END)!=0) return false;
        const long length=std::ftell(fp);
        if(length<=0 || length>512L*1024*1024 || std::fseek(fp,0,SEEK_SET)!=0) return false;
        std::vector<uint8_t> blob(static_cast<size_t>(length));
        if(std::fread(blob.data(),1,blob.size(),fp)!=blob.size()) return false;
        if(rknn_init(&sharedModel_->root,blob.data(),static_cast<uint32_t>(blob.size()),0,nullptr)<0) {
            sharedModel_->root=0; Logger::error("RKNN","rknn_init failed"); return false;
        }
        sharedModel_->path=modelPath_;
    }
    if(sharedModel_->path!=modelPath_) return false;
    rknn_context ctx=0;
    if(rknn_dup_context(&sharedModel_->root,&ctx)<0) {
        Logger::error("RKNN","rknn_dup_context failed; no silent independent-model fallback");return false;
    }
    ctx_=reinterpret_cast<void*>(static_cast<uintptr_t>(ctx));
    Logger::info("RKNN","duplicated context; shared model="+modelPath_);

    rknn_sdk_version ver;
    std::memset(&ver, 0, sizeof(ver));
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == 0) {
        Logger::info("RKNN", std::string("sdk=") + ver.api_version + " driver=" + ver.drv_version);
    }

    /* 3. 查询输入输出数量 */
    rknn_input_output_num ioNum;
    std::memset(&ioNum, 0, sizeof(ioNum));
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ioNum, sizeof(ioNum)) < 0) {
        Logger::error("RKNN", "RKNN_QUERY_IN_OUT_NUM failed");
        return false;
    }
    numInput_ = static_cast<int>(ioNum.n_input);
    numOutput_ = static_cast<int>(ioNum.n_output);

    /* 4. 查询输入属性（模型输入尺寸由模型决定，不写死） */
    if (numInput_ != 1 || numOutput_ != 9) {
        Logger::error("RKNN", "model exposes no input");
        return false;
    }
    rknn_tensor_attr inAttr;
    std::memset(&inAttr, 0, sizeof(inAttr));
    inAttr.index = 0;
    if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &inAttr, sizeof(inAttr)) < 0) {
        Logger::error("RKNN", "RKNN_QUERY_INPUT_ATTR failed");
        return false;
    }
    if(inAttr.n_dims!=4 || inAttr.dims[0]!=1 ||
       (inAttr.fmt!=RKNN_TENSOR_NHWC && inAttr.fmt!=RKNN_TENSOR_NCHW)) return false;
    const bool nhwc=inAttr.fmt==RKNN_TENSOR_NHWC;
    if(inAttr.dims[nhwc?3:1]!=3) return false;
    modelH_ = static_cast<int>(inAttr.dims[nhwc?1:2]);
    modelW_ = static_cast<int>(inAttr.dims[nhwc?2:3]);
    if (modelW_ < 2 || modelH_ < 2 || modelW_ > 4096 || modelH_ > 4096) {
        Logger::error("RKNN", "unexpected model input dims");
        return false;
    }
    Logger::info("RKNN", "model loaded: " + modelPath_ + ", input=" + std::to_string(modelW_) + "x" + std::to_string(modelH_) +
                             " fmt=" + std::to_string(inAttr.fmt) + " type=" + std::to_string(inAttr.type) +
                             ", outputs=" + std::to_string(numOutput_));
    rknn_tensor_attr nativeAttr{};
    if(rknn_query(ctx,RKNN_QUERY_NATIVE_INPUT_ATTR,&nativeAttr,sizeof(nativeAttr))==0)
        Logger::info("RKNN","native input fmt="+std::to_string(nativeAttr.fmt)+" type="+std::to_string(nativeAttr.type)+
            " stride="+std::to_string(nativeAttr.w_stride)+" bytes="+std::to_string(nativeAttr.size_with_stride)+
            " zp="+std::to_string(nativeAttr.zp)+" scale="+std::to_string(nativeAttr.scale));

    /* 5. 解析 9 个分离输出：按通道数分类，按网格高度配对成尺度 */
    struct Pending { int dflIdx = -1; int clsIdx = -1; int h = 0; int w = 0; };
    std::vector<Pending> pending;
    for (int i = 0; i < numOutput_; ++i) {
        rknn_tensor_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.index = static_cast<uint32_t>(i);
        if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) < 0) {
            Logger::error("RKNN", "RKNN_QUERY_OUTPUT_ATTR failed for output " + std::to_string(i));
            return false;
        }
        if(attr.n_dims!=4 || attr.dims[0]!=1 || attr.fmt!=RKNN_TENSOR_NCHW) {
            Logger::error("RKNN","only contiguous NCHW detection outputs are supported");return false;
        }
        const int c=static_cast<int>(attr.dims[1]), h=static_cast<int>(attr.dims[2]), w=static_cast<int>(attr.dims[3]);
        if(c<1||c>1024||h<1||w<1||h>modelH_||w>modelW_ ||
           static_cast<size_t>(c)*static_cast<size_t>(h)*static_cast<size_t>(w)!=attr.n_elems) return false;
        outputMeta_.push_back({attr.n_elems,static_cast<int>(attr.type),static_cast<int>(attr.qnt_type),attr.scale,attr.zp});
        Logger::info("RKNN","output="+std::to_string(i)+" type="+std::to_string(attr.type)+
            " scale="+std::to_string(attr.scale)+" zp="+std::to_string(attr.zp));
        if(c==1) continue; // score-sum branch not used as objectness
        auto it=std::find_if(pending.begin(),pending.end(),[&](const Pending& p){return p.h==h&&p.w==w;});
        if(it==pending.end()){pending.push_back(Pending{});it=std::prev(pending.end());it->h=h;it->w=w;}
        if(c==4*kDflBins){if(it->dflIdx>=0)return false;it->dflIdx=i;}
        else {if(c!=80 || it->clsIdx>=0) {Logger::error("RKNN","expected COCO80 class branch");return false;}it->clsIdx=i;classNum_=c;}
    }

    for (const auto& p : pending) {
        if (p.dflIdx < 0 || p.clsIdx < 0) continue;
        Scale s;
        s.h = p.h; s.w = p.w; s.dflIdx = p.dflIdx; s.clsIdx = p.clsIdx;
        scales_.push_back(s);
    }
    if (scales_.size()!=3) {
        Logger::error("RKNN", "cannot pair DFL/class outputs; unsupported model layout");
        return false;
    }
    /* 大尺度在前（stride 小），顺序稳定 */
    std::sort(scales_.begin(), scales_.end(),
              [](const Scale& a, const Scale& b) { return a.h > b.h; });

    std::string layout;
    for (const auto& s : scales_) {
        layout += " [H=" + std::to_string(s.h) + " dfl=" + std::to_string(s.dflIdx) +
                  " cls=" + std::to_string(s.clsIdx) + "]";
    }
    Logger::info("RKNN", "detection head scales:" + layout +
                             ", classes=" + std::to_string(classNum_) +
                             ", conf_thresh=" + std::to_string(confThresh_));
    return true;
}

bool RknnInferenceEngine::infer(const Frame& frame, std::vector<Detection>& dets) {
    dets.clear();
    if (!ctx_) return false;

    const size_t expected = static_cast<size_t>(modelW_) * static_cast<size_t>(modelH_) * 3;
    const bool dma=frame.dmaFd>=0 && frame.bufferOwner && frame.mappedData && frame.pixelFormat==Frame::PixelFormat::RGB888;
    if(inputMode_>=0&&inputMode_!=static_cast<int>(dma)){
        Logger::error("RKNN","cannot mix DMA and CPU input APIs on one context");return false;
    }
    inputMode_=static_cast<int>(dma);
    if (frame.width != modelW_ || frame.height != modelH_ || (!dma && frame.data.size() != expected)) {
        Logger::error("RKNN", "unexpected input frame " + std::to_string(frame.width) + "x" +
                                  std::to_string(frame.height) + " data=" + std::to_string(frame.data.size()) +
                                  " (expect " + std::to_string(modelW_) + "x" + std::to_string(modelH_) +
                                  " RGB888)");
        return false;
    }

    rknn_context ctx = static_cast<rknn_context>(reinterpret_cast<uintptr_t>(ctx_));

    const auto started=std::chrono::steady_clock::now();
    struct InputMemory {rknn_context ctx;rknn_tensor_mem* mem=nullptr;
        ~InputMemory(){if(mem)rknn_destroy_mem(ctx,mem);}} inputMemory{ctx};
    int ret=0;
    if(dma){
        if(frame.widthStride<modelW_||frame.dmaBytes<static_cast<size_t>(frame.widthStride)*modelH_*3)return false;
        rknn_tensor_attr attr{};attr.index=0;
        if(rknn_query(ctx,RKNN_QUERY_INPUT_ATTR,&attr,sizeof(attr))<0)return false;
        attr.type=RKNN_TENSOR_UINT8;attr.fmt=RKNN_TENSOR_NHWC;attr.pass_through=0;
        attr.w_stride=static_cast<uint32_t>(frame.widthStride);
        inputMemory.mem=rknn_create_mem_from_fd(ctx,frame.dmaFd,frame.mappedData,static_cast<uint32_t>(frame.dmaBytes),0);
        if(!inputMemory.mem){Logger::error("RKNN","DMA fd import failed");return false;}
        ret=rknn_set_io_mem(ctx,inputMemory.mem,&attr);
    }else{
        rknn_input input{};input.type=RKNN_TENSOR_UINT8;input.size=static_cast<uint32_t>(expected);
        input.fmt=RKNN_TENSOR_NHWC;input.buf=const_cast<uint8_t*>(frame.data.data());
        ret=rknn_inputs_set(ctx,1,&input);
    }
    if(ret<0){Logger::error("RKNN","input binding failed: "+std::to_string(ret));return false;}

    const auto afterInput=std::chrono::steady_clock::now();
    /* 2. 前向计算 */
    ret = rknn_run(ctx, nullptr);
    if (ret < 0) {
        Logger::error("RKNN", "rknn_run failed: " + std::to_string(ret));
        return false;
    }

    const auto afterRun=std::chrono::steady_clock::now();
    // Native INT8/UINT8 avoids converting every output element. Unsupported types use SDK float output.
    std::vector<rknn_output> outputs(static_cast<size_t>(numOutput_));
    for (int i = 0; i < numOutput_; ++i) {
        std::memset(&outputs[static_cast<size_t>(i)], 0, sizeof(rknn_output));
        outputs[static_cast<size_t>(i)].index = static_cast<uint32_t>(i);
        const auto& meta=outputMeta_[static_cast<size_t>(i)];
        const bool affine=meta.qnt==RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
            (meta.type==RKNN_TENSOR_INT8 || meta.type==RKNN_TENSOR_UINT8);
        outputs[static_cast<size_t>(i)].want_float = nativeOutputs_ && affine ? 0 : 1;
    }
    ret = rknn_outputs_get(ctx, static_cast<uint32_t>(numOutput_), outputs.data(), nullptr);
    if (ret < 0) {
        Logger::error("RKNN", "rknn_outputs_get failed: " + std::to_string(ret));
        return false;
    }

    // Scope guard releases SDK buffers even if postprocessing throws.
    struct OutputGuard { rknn_context ctx; std::vector<rknn_output>& out;
        ~OutputGuard(){rknn_outputs_release(ctx,static_cast<uint32_t>(out.size()),out.data());}
    } guard{ctx,outputs};
    const auto afterGet=std::chrono::steady_clock::now();
    std::vector<TensorView> bufs;bufs.reserve(outputs.size());
    for(size_t i=0;i<outputs.size();++i){
        const auto& m=outputMeta_[i];
        auto type=outputs[i].want_float?TensorView::Type::Float32:
            m.type==RKNN_TENSOR_INT8?TensorView::Type::Int8:TensorView::Type::Uint8;
        bufs.emplace_back(outputs[i].buf,outputs[i].size,m.count,type,m.scale,m.zp);
    }
    decode(bufs,dets);
    const auto afterDecode=std::chrono::steady_clock::now();
    if(!debugOnce_ || frame.frameIndex%100==0){
        debugOnce_=true;
        auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
        Logger::info("RKNN_TIMING","stream="+std::to_string(frame.streamId)+
            " dma_input="+std::to_string(dma)+" native_outputs="+std::to_string(nativeOutputs_)+
            " input_ms="+std::to_string(ms(started,afterInput))+" run_ms="+std::to_string(ms(afterInput,afterRun))+
            " get_ms="+std::to_string(ms(afterRun,afterGet))+" decode_ms="+std::to_string(ms(afterGet,afterDecode)));
    }
    return true;
}

void RknnInferenceEngine::decode(const std::vector<TensorView>& outBufs,
                                 std::vector<Detection>& dets) const {
    for (const auto& sc : scales_) {
        if (sc.dflIdx >= static_cast<int>(outBufs.size()) || sc.clsIdx >= static_cast<int>(outBufs.size())) continue;
        const auto& dfl = outBufs[static_cast<size_t>(sc.dflIdx)];
        const auto& cls = outBufs[static_cast<size_t>(sc.clsIdx)];

        const int H = sc.h;
        const int W = sc.w;
        const int HW = H * W;
        const float strideX = static_cast<float>(modelW_) / static_cast<float>(W);
        const float strideY = static_cast<float>(modelH_) / static_cast<float>(H);

        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int pos = y * W + x;

                /* 1. 最大类别分数（类别分数已由运行时反量化为 0..1 概率） */
                float maxScore = 0.0f;
                int maxId = -1;
                cls.bestClass(static_cast<size_t>(HW),static_cast<size_t>(pos),classNum_,maxId,maxScore);
                if (maxId < 0 || maxScore < confThresh_) continue;

                /* 2. DFL 解码：4 个方向各自 softmax(16) 求期望距离 */
                float dist[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int d = 0; d < 4; ++d) {
                    const size_t base = static_cast<size_t>(d * kDflBins) * HW + pos;
                    float maxv = -1e30f;
                    for (int i = 0; i < kDflBins; ++i) {
                        const float v = dfl.value(base + static_cast<size_t>(i) * HW);
                        if (v > maxv) maxv = v;
                    }
                    float sum = 0.0f, acc = 0.0f;
                    for (int i = 0; i < kDflBins; ++i) {
                        const float e = std::exp(dfl.value(base + static_cast<size_t>(i) * HW) - maxv);
                        sum += e;
                        acc += e * static_cast<float>(i);
                    }
                    dist[d] = sum > 0.0f ? acc / sum : 0.0f;
                }

                /* 3. 网格中心 + 距离 → 绝对坐标（模型输入坐标系），并裁剪到画面内 */
                const float cx = static_cast<float>(x) + 0.5f;
                const float cy = static_cast<float>(y) + 0.5f;
                const float x1 = std::max(0.0f, (cx - dist[0]) * strideX);
                const float y1 = std::max(0.0f, (cy - dist[1]) * strideY);
                const float x2 = std::min(static_cast<float>(modelW_), (cx + dist[2]) * strideX);
                const float y2 = std::min(static_cast<float>(modelH_), (cy + dist[3]) * strideY);
                if (x2 - x1 <= 1.0f || y2 - y1 <= 1.0f) continue;

                Detection det;
                det.box = Box{x1, y1, x2 - x1, y2 - y1};
                det.classId = maxId;
                det.confidence = maxScore;
                dets.push_back(det);
            }
        }
    }

    /* 4. 置信度降序 + 截断（NMS 由流水线统一执行） */
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    if (dets.size() > static_cast<size_t>(kMaxCandidates)) dets.resize(static_cast<size_t>(kMaxCandidates));
}

} // namespace msad
