/*
 * motion_eval.cpp — 动态场景检测测评（离线工具，不参与主程序运行）
 *
 * 用法: motion_eval <model.rknn> <video> [outdir] [maxFrames]
 *
 * 做什么：用真实链路（MPP 硬解 → RGA 预处理 → RKNN 推理 + 手写 NMS）逐帧跑一段
 *         动态视频，输出可复核的量化指标与可视化证据。
 *
 * 为什么需要它：板卡上的双路基准用的是**静态重复样本**（person_h264.mp4 源自一张告警图），
 *         只能证明"能跑通"，不能说明"在人走动时检测是否稳定"。本工具补的是后者。
 *
 * 输出（outdir/）：
 *   detections.csv   逐帧明细（帧号、是否唯一帧、目标数、最高分类别/置信度/框）
 *   summary.json     汇总指标
 *   frame_XXXX.png   带框可视化证据（最多 6 张，取自不同时间段）
 *
 * 说明：录制素材里 2/3 是 ffmpeg 补出来的重复帧，因此所有比率类指标都按
 *       "唯一帧"（内容哈希变化）统计，避免重复帧把"稳定性"算得虚高。
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>

#include "msad/Logger.h"
#include "msad/Nms.h"
#include "msad/PngWriter.h"
#include "msad/RkMppSource.h"
#include "msad/RealComponents.h"

using namespace msad;

namespace {

constexpr int   kPersonClass = 0;      /* COCO: person */
constexpr float kConfThresh  = 0.25f;
constexpr float kNmsThresh   = 0.45f;
constexpr int   kMaxEvidence = 6;      /* 最多导出多少张可视化截图 */

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

float iou(const Box& a, const Box& b) {
    const float x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    const float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

struct FrameRec {
    uint64_t idx = 0;
    bool     unique = true;
    int      nDets = 0;
    int      bestClass = -1;
    float    bestConf = 0.0f;
    float    personConf = 0.0f;   /* 该帧"人"的最高置信度（与全体最高分开统计，避免互相污染） */
    bool     hasPerson = false;
    Box      person;          /* 若有多人，取置信度最高者 */
    std::vector<Detection> dets;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s MODEL VIDEO [OUTDIR] [maxFrames]\n", argv[0]);
        return 2;
    }
    const std::string modelPath = argv[1];
    const std::string video     = argv[2];
    const std::string outdir    = (argc > 3) ? argv[3] : std::string("outputs/motion_eval");
    int maxFrames=900;
    try {
        if(argc>4){size_t used=0;maxFrames=std::stoi(argv[4],&used);if(used!=std::strlen(argv[4]))throw std::invalid_argument("frame count");}
        if(maxFrames<1||maxFrames>10000)throw std::invalid_argument("frame count");
    } catch(const std::exception&) {
        std::fprintf(stderr,"maxFrames must be an integer in 1..10000\n");return 2;
    }

    if(!std::filesystem::create_directory(outdir)) {
        std::fprintf(stderr,"output directory must be new: %s\n",outdir.c_str());return 2;
    }
    std::ofstream predictions(outdir+"/predictions.jsonl");
    std::ofstream timings(outdir+"/latency.csv");
    std::ofstream labels(outdir+"/annotation-template.jsonl");
    std::ofstream selected(outdir+"/annotation-predictions.jsonl");
    if(!predictions||!timings||!labels||!selected)return 2;
    timings<<"frame,capture_ms,preprocess_ms,infer_nms_ms,total_with_audit_ms\n";

    RkMppSource source;
    if (!source.open(video)) { std::fprintf(stderr, "cannot open video: %s\n", video.c_str()); return 1; }
    RgaFrameProcessor pre(640, 640, false, true);
    if (!pre.init()) return 1;
    RknnInferenceEngine engine(modelPath, kConfThresh);
    if (!engine.init()) return 1;

    std::vector<FrameRec> recs;
    recs.reserve(static_cast<size_t>(maxFrames));

    uint64_t prevHash = 0;
    int uniqueCount = 0, detectedUnique = 0, multiPersonUnique = 0;
    int savedEvidence = 0;
    int lastEvidenceUnique = -1000;
    int totalPersonBoxes = 0;
    double confSum = 0.0;
    float confMin = 1e9f, confMax = -1e9f;
    int run = 0, bestRun = 0, bestGap = 0;
    int lastDetectedUnique = -1;
    double jitterSum = 0.0; int jitterN = 0;
    Box prevPerson; bool havePrevPerson = false;
    int classHist[80] = {0};

    for (int f = 0; f < maxFrames; ++f) {
        const auto started=std::chrono::steady_clock::now();
        Frame raw;
        if (!source.readFrame(raw)) { Logger::warn("motion_eval", "readFrame ended at " + std::to_string(f)); break; }
        const auto captured=std::chrono::steady_clock::now();

        Frame proc;
        if (!pre.process(std::move(raw), proc)) { Logger::warn("motion_eval", "preprocess failed at " + std::to_string(f)); continue; }
        const auto preprocessed=std::chrono::steady_clock::now();

        /*
         * 取 CPU 可见像素：走 dma_input 时像素在 DMA 缓冲里，proc.data 不是逐帧内容，
         * 必须用工程提供的 Frame::copyRgb（它会同步 DMA 缓冲）。这两个用途都需要真实像素：
         * 内容哈希（判断 ffmpeg 补帧）与可视化截图。
         */
        std::vector<uint8_t> pixels;
        if (proc.copyRgb) {
            if (!proc.copyRgb(pixels)) { Logger::warn("motion_eval", "copyRgb failed at " + std::to_string(f)); continue; }
        } else {
            pixels = proc.data;
        }
        if (pixels.empty()) { Logger::warn("motion_eval", "no CPU pixels at " + std::to_string(f)); continue; }

        const uint64_t h = fnv1a(pixels.data(), pixels.size());
        const bool unique = (h != prevHash);
        prevHash = h;

        std::vector<Detection> dets;
        const auto inferStarted=std::chrono::steady_clock::now();
        if (!engine.infer(proc, dets)) { Logger::warn("motion_eval", "infer failed at " + std::to_string(f)); continue; }
        dets = nms(dets, kNmsThresh);
        const auto finished=std::chrono::steady_clock::now();
        auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
        timings<<f<<','<<ms(started,captured)<<','<<ms(captured,preprocessed)<<','<<ms(inferStarted,finished)<<','<<ms(started,finished)<<'\n';
        std::ostringstream row;
        row<<"{\"frame\":"<<f<<",\"boxes\":[";
        bool first=true;
        for(const auto& d:dets){
            if(!first)row<<',';
            first=false;
            row<<"{\"class_id\":"<<d.classId<<",\"score\":"<<d.confidence
                <<",\"xywh\":["<<d.box.x<<','<<d.box.y<<','<<d.box.w<<','<<d.box.h<<"]}";
        }
        row<<"]}\n";
        predictions<<row.str();
        // Unannotated images allow independent review without showing detections.
        if(f<120 && f%20==0){
            char imagePath[512];std::snprintf(imagePath,sizeof(imagePath),"%s/label_frame_%04d.png",outdir.c_str(),f);
            Frame shot=proc;shot.data=pixels;
            if(!PngWriter::saveFrameAsPng(shot,imagePath,{}))return 1;
            labels<<"{\"frame\":"<<f<<",\"reviewed\":false,\"boxes\":[]}\n";
            selected<<row.str();
        }

        FrameRec r;
        r.idx = static_cast<uint64_t>(f);
        r.unique = unique;
        r.nDets = static_cast<int>(dets.size());
        for (const auto& d : dets) {
            if (d.classId >= 0 && d.classId < 80) classHist[d.classId]++;
            if (d.classId == kPersonClass) {
                totalPersonBoxes++;
                if (d.confidence > r.personConf) { r.personConf = d.confidence; r.person = d.box; r.hasPerson = true; }
            }
            if (d.confidence > r.bestConf) { r.bestConf = d.confidence; r.bestClass = d.classId; }
        }
        r.dets = dets;

        if (unique) {
            ++uniqueCount;
            if (r.hasPerson) {
                ++detectedUnique;
                confSum += r.personConf;
                confMin = std::min(confMin, r.personConf);
                confMax = std::max(confMax, r.personConf);
                if (r.nDets >= 2) ++multiPersonUnique;
                /* 抖动：相邻"有人的唯一帧"之间框的 IoU（越接近 1 越稳） */
                if (havePrevPerson) { jitterSum += iou(prevPerson, r.person); ++jitterN; }
                prevPerson = r.person; havePrevPerson = true;
                if (lastDetectedUnique >= 0 && uniqueCount - lastDetectedUnique == 1) ++run; else run = 1;
                bestRun = std::max(bestRun, run);
                if (lastDetectedUnique >= 0) bestGap = std::max(bestGap, uniqueCount - lastDetectedUnique - 1);
                lastDetectedUnique = uniqueCount;
                if (savedEvidence < kMaxEvidence && uniqueCount - lastEvidenceUnique >= 25) {
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/frame_%04d_u%04d.png", outdir.c_str(), f, uniqueCount);
                    Frame shot = proc;              /* 仅为截图复制一份 CPU 像素，不是每帧开销 */
                    shot.data = pixels;
                    if (PngWriter::saveFrameAsPng(shot, path, dets)) { ++savedEvidence; lastEvidenceUnique = uniqueCount; }
                }
            } else {
                prevPerson = Box{};
                havePrevPerson = false;
                run = 0;
            }
        }

        recs.push_back(std::move(r));
        if ((f + 1) % 100 == 0) Logger::info("motion_eval", "processed " + std::to_string(f + 1) + " frames");
    }
    source.close();

    /* ---- 落盘：逐帧明细 ---- */
    const std::string csv = outdir + "/detections.csv";
    if (FILE* fp = std::fopen(csv.c_str(), "w")) {
        std::fprintf(fp, "frame,unique,n_dets,best_class,best_conf,person,person_conf,px,py,pw,ph\n");
        for (const auto& r : recs) {
            std::fprintf(fp, "%llu,%d,%d,%d,%.4f,%d,%.4f,%.1f,%.1f,%.1f,%.1f\n",
                         static_cast<unsigned long long>(r.idx), r.unique ? 1 : 0, r.nDets,
                         r.bestClass, r.bestConf, r.hasPerson ? 1 : 0, r.personConf,
                         r.person.x, r.person.y, r.person.w, r.person.h);
        }
        std::fclose(fp);
    }

    const double detectRate = uniqueCount > 0 ? static_cast<double>(detectedUnique) / uniqueCount : 0.0;
    const double confMean   = detectedUnique > 0 ? confSum / detectedUnique : 0.0;
    const double jitterMean = jitterN > 0 ? jitterSum / jitterN : 0.0;

    /* ---- 落盘：汇总 ---- */
    const std::string js = outdir + "/summary.json";
    const bool completed=recs.size()==static_cast<size_t>(maxFrames);
    bool summaryWritten=false;
    if (FILE* fp = std::fopen(js.c_str(), "w")) {
        std::fprintf(fp, "{\n");
        std::fprintf(fp, "  \"completed\": %s,\n",completed?"true":"false");
        std::fprintf(fp, "  \"video\": \"%s\",\n", video.c_str());
        std::fprintf(fp, "  \"model\": \"%s\",\n", modelPath.c_str());
        std::fprintf(fp, "  \"conf_thresh\": %.2f,\n  \"nms_thresh\": %.2f,\n", kConfThresh, kNmsThresh);
        std::fprintf(fp, "  \"frames_processed\": %zu,\n", recs.size());
        std::fprintf(fp, "  \"frames_unique\": %d,\n", uniqueCount);
        std::fprintf(fp, "  \"person_frames_unique\": %d,\n", detectedUnique);
        std::fprintf(fp, "  \"person_detect_rate\": %.4f,\n", detectRate);
        std::fprintf(fp, "  \"person_conf_min\": %.4f,\n", detectedUnique ? confMin : 0.0f);
        std::fprintf(fp, "  \"person_conf_mean\": %.4f,\n", confMean);
        std::fprintf(fp, "  \"person_conf_max\": %.4f,\n", detectedUnique ? confMax : 0.0f);
        std::fprintf(fp, "  \"person_boxes_total\": %d,\n", totalPersonBoxes);
        std::fprintf(fp, "  \"frames_ge2_objects\": %d,\n", multiPersonUnique);
        std::fprintf(fp, "  \"longest_detected_run_unique\": %d,\n", bestRun);
        std::fprintf(fp, "  \"longest_gap_unique\": %d,\n", bestGap);
        std::fprintf(fp, "  \"mean_box_iou_consecutive\": %.4f,\n", jitterMean);
        std::fprintf(fp, "  \"iou_samples\": %d,\n", jitterN);
        std::fprintf(fp, "  \"evidence_png\": %d\n", savedEvidence);
        std::fprintf(fp, "}\n");
        summaryWritten=std::ferror(fp)==0;
        if(std::fclose(fp)!=0)summaryWritten=false;
    }

    std::printf("\n===== 动态测评结果 =====\n");
    std::printf("处理帧数            : %zu\n", recs.size());
    std::printf("唯一帧数            : %d（其余为录制补帧）\n", uniqueCount);
    std::printf("检出 person 的唯一帧 : %d / %d = %.1f%%\n", detectedUnique, uniqueCount, detectRate * 100.0);
    std::printf("person 置信度        : min %.3f  平均 %.3f  max %.3f\n",
                detectedUnique ? confMin : 0.0f, confMean, detectedUnique ? confMax : 0.0f);
    std::printf("person 框总数        : %d\n", totalPersonBoxes);
    std::printf("最长连续检出(唯一帧) : %d ；最长丢失间隔(唯一帧): %d\n", bestRun, bestGap);
    std::printf("相邻帧框平均 IoU     : %.3f（样本 %d）\n", jitterMean, jitterN);
    std::printf("可视化证据           : %d 张 → %s/frame_*.png\n\n", savedEvidence, outdir.c_str());
    predictions.flush();timings.flush();labels.flush();selected.flush();
    return completed&&summaryWritten&&predictions.good()&&timings.good()&&labels.good()&&selected.good()?0:1;
}
