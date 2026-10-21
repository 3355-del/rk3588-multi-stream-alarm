/*
 * Config.cpp — 配置加载实现
 */
#include "msad/Config.h"

#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <limits>
#include <set>

#include "msad/Json.h"
#include "msad/Logger.h"

namespace msad {

static bool readFile(const std::string& path, std::string& out, std::string& err) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        err = "cannot open config file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

uint64_t AppConfig::estimatedBufferBytes() const {
    if (frameWidth < 2 || frameHeight < 2 || frameWidth > 4096 || frameHeight > 4096 ||
        queueCapacity < 1 || queueCapacity > 64 || alarmQueueCapacity < 1 || alarmQueueCapacity > 64 ||
        streams.size() > 4) return std::numeric_limits<uint64_t>::max();
    const uint64_t pixels = static_cast<uint64_t>(frameWidth) * static_cast<uint64_t>(frameHeight);
    const uint64_t frameBytes = hardware ? std::max(pixels * 3,
        static_cast<uint64_t>(std::max(captureWidth,0))*static_cast<uint64_t>(std::max(captureHeight,0))*2) : pixels * 3 / 2;
    // All three queues, five in-flight frames per stream, IO queue and current IO frame.
    const uint64_t slots = streams.size() * (static_cast<uint64_t>(queueCapacity) * 3 + 5) +
                           static_cast<uint64_t>(alarmQueueCapacity) + 1;
    const uint64_t rgb = pixels * 3;
    const uint64_t raw = rgb + static_cast<uint64_t>(frameHeight);
    const uint64_t zlib = raw + 5 * ((raw + 65534) / 65535) + 6;
    const uint64_t png = zlib + 57;
    // Reserved PNG buffers coexist. Margin covers ordinary metadata/allocator overhead,
    // not an OS-enforced process memory limit.
    uint64_t decoderBudget=0;
    if(hardware)for(const auto& stream:streams)if(stream.sourceUri.rfind("/dev/video",0)!=0)decoderBudget+=96ULL*1024*1024;
    return slots * frameBytes + rgb + raw + zlib + png + 32ULL * 1024 * 1024 + decoderBudget;
}

bool AppConfig::validate(std::string& err) const {
    err.clear();
    Frame f; f.width = frameWidth; f.height = frameHeight;
    if (!f.validDimensions()) { err = "frame dimensions must be even and in [2,4096]"; return false; }
    if (queueCapacity < 1 || queueCapacity > 64 || alarmQueueCapacity < 1 || alarmQueueCapacity > 64 || fps < 1 || fps > 240 || runSeconds < 0) {
        err = "queue_capacity and alarm_queue_capacity: 1..64, fps: 1..240, run_seconds: >=0"; return false;
    }
    if (streams.empty() || streams.size() > 4) { err = "streams count must be 1..4"; return false; }
    if(inferenceConcurrency<1 || inferenceConcurrency>2) {err="inference_concurrency must be 1..2";return false;}
    if(!std::isfinite(confThresh)||confThresh<=0||confThresh>1||!std::isfinite(nmsThresh)||nmsThresh<0||nmsThresh>1) {err="invalid confidence/NMS threshold";return false;}
    const uint64_t bytes = estimatedBufferBytes();
    if (bytes > 512ULL * 1024 * 1024) { err = "frame/queue configuration exceeds 512 MiB buffer budget (including alarm IO and PNG)"; return false; }
    if (screenshotDir.empty() || alarm.host.empty() || alarm.port < 1 || alarm.port > 65535) {
        err = "invalid screenshot directory or alarm endpoint"; return false;
    }
    if (hardware) {
        if (modelPath.empty()) { err = "model_path is required when hardware=true"; return false; }
        Frame cap; cap.width = captureWidth; cap.height = captureHeight;
        if (!cap.validDimensions()) { err = "capture_width/capture_height must be even and in [2,4096]"; return false; }
        std::set<std::string> devices;
        for (const auto& s : streams) {
            if(s.sourceUri.empty()||s.sourceUri.rfind("mock://",0)==0){err="hardware source must be a file, V4L2 device or RTSP URL";return false;}
            if(s.sourceUri.rfind("/dev/video",0)==0){
                if (!devices.insert(s.sourceUri).second) { err = "duplicate capture device"; return false; }
            }else if(queueCapacity>16){err="MPP capture queue must be <=16 to reserve decoder reference surfaces";return false;}
        }
    }
    std::set<int> ids;
    for (const auto& s : streams) {
        if (s.id < 0 || !ids.insert(s.id).second) { err = "stream ids must be unique and nonnegative"; return false; }
        std::set<int> regions;
        for (const auto& r : s.regions) {
            if (r.id < 0 || !regions.insert(r.id).second || r.polygon.size() < 3 || r.polygon.size() > 256) {
                err = "invalid region id or polygon vertex count (3..256)"; return false;
            }
            double area = 0;
            for (size_t i = 0; i < r.polygon.size(); ++i) {
                const auto& a = r.polygon[i]; const auto& b = r.polygon[(i+1)%r.polygon.size()];
                if (!std::isfinite(a.x) || !std::isfinite(a.y) || std::fabs(a.x) > 1e6f || std::fabs(a.y) > 1e6f) {
                    err = "invalid polygon coordinate"; return false;
                }
                area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
            }
            if (std::fabs(area) < 1e-6) { err = "degenerate polygon"; return false; }
            for (int c : r.classes) if (c < 0 || c >= (hardware?kNumCocoClassNames:kNumClassNames)) {
                err = hardware?"hardware class id must be 0..79":"mock class id must be 0..4"; return false;
            }
        }
    }
    return true;
}

bool AppConfig::load(const std::string& jsonPath, AppConfig& destination, std::string& err) {
    AppConfig out;
    err.clear();
    std::string text;
    if (!readFile(jsonPath, text, err)) return false;

    Json parsed;
    if (!Json::parse(text, parsed, err)) {
        err = "config parse error: " + err;
        return false;
    }
    const Json& root = parsed;
    if (!root.isObject()) {
        err = "config root must be an object";
        return false;
    }

    // Missing keys use defaults; present keys must have the declared type.
    auto integers = [&](const Json& obj, std::initializer_list<const char*> keys) {
        for (const auto* key : keys) if (obj.has(key)) {
            const Json& v = obj[key];
            const double d = v.asNumber();
            if (!v.isNumber() || !std::isfinite(d) || std::floor(d) != d ||
                d < std::numeric_limits<int>::min() || d > std::numeric_limits<int>::max()) {
                err = std::string(key) + " must be an integer in int range"; return false;
            }
        }
        return true;
    };
    auto strings = [&](const Json& obj, std::initializer_list<const char*> keys) {
        for (const auto* key : keys) if (obj.has(key) &&
            (!obj[key].isString() || obj[key].asString().find(char(0)) != std::string::npos)) {
            err = std::string(key) + " must be a string without NUL"; return false;
        }
        return true;
    };
    if (!integers(root, {"frame_width","frame_height","queue_capacity","alarm_queue_capacity","fps","run_seconds","capture_width","capture_height","inference_concurrency"}) ||
        !strings(root, {"screenshot_dir","model_path"})) return false;
    out.frameWidth    = root["frame_width"].asInt(1280);
    out.frameHeight   = root["frame_height"].asInt(720);
    out.queueCapacity = root["queue_capacity"].asInt(8);
    out.alarmQueueCapacity = root["alarm_queue_capacity"].asInt(8);
    out.fps           = root["fps"].asInt(30);
    out.runSeconds    = root["run_seconds"].asInt(30);
    out.screenshotDir = root["screenshot_dir"].asString("screenshots");

    /* RK3588 真实硬件模式（缺省 false，保持 mock 行为不变） */
    if (root.has("hardware") && !root["hardware"].isBool()) { err = "hardware must be boolean"; return false; }
    out.hardware     = root["hardware"].asBool(false);
    out.inferenceConcurrency=root["inference_concurrency"].asInt(2);
    if(root.has("native_outputs")&&!root["native_outputs"].isBool()){err="native_outputs must be boolean";return false;}
    out.nativeOutputs=root["native_outputs"].asBool(true);
    if(root.has("dma_input")&&!root["dma_input"].isBool()){err="dma_input must be boolean";return false;}
    out.dmaInput=root["dma_input"].asBool(true);
    out.modelPath    = root["model_path"].asString("");
    out.captureWidth = root["capture_width"].asInt(640);
    out.captureHeight= root["capture_height"].asInt(480);
    for (const char* key : {"conf_thresh", "nms_thresh"}) {
        if (root.has(key) && (!root[key].isNumber() || !std::isfinite(root[key].asNumber()))) {
            err = std::string(key) + " must be a finite number"; return false;
        }
    }
    out.confThresh = root["conf_thresh"].asNumber(0.25);
    out.nmsThresh  = root["nms_thresh"].asNumber(0.45);
    if (out.confThresh <= 0.0 || out.confThresh > 1.0 || out.nmsThresh <= 0.0 || out.nmsThresh > 1.0) {
        err = "conf_thresh and nms_thresh must be in (0,1]"; return false;
    }
    if (root.has("camera_flip_180") && !root["camera_flip_180"].isBool()) {
        err = "camera_flip_180 must be boolean"; return false;
    }
    out.cameraFlip180 = root["camera_flip_180"].asBool(false);

    if (root.has("alarm")) {
        if (!root["alarm"].isObject()) { err = "alarm must be an object"; return false; }
        if (!integers(root["alarm"], {"port"}) || !strings(root["alarm"], {"host"})) return false;
        out.alarm.host = root["alarm"]["host"].asString("127.0.0.1");
        out.alarm.port = root["alarm"]["port"].asInt(9000);
        if (root["alarm"].has("reliable") && !root["alarm"]["reliable"].isBool()) { err="alarm.reliable must be boolean"; return false; }
        if (!strings(root["alarm"], {"outbox_dir"})) return false;
        out.alarm.reliable = root["alarm"]["reliable"].asBool(false);
        out.alarm.outboxDir = root["alarm"]["outbox_dir"].asString("alarm_outbox");
        if (out.alarm.reliable && out.alarm.outboxDir.empty()) { err="outbox_dir is empty"; return false; }
    }

    /* 视频流配置（至少 1 路，最多 4 路） */
    const Json& streams = root["streams"];
    if (!streams.isArray() || streams.size() == 0 || streams.size() > 4) {
        err = "config must contain non-empty 'streams' array";
        return false;
    }
    for (size_t i = 0; i < streams.size(); ++i) {
        StreamConfig sc;
        const Json& s = streams[i];
        if (!s.isObject()) { err = "stream must be an object"; return false; }
        if (!integers(s, {"id"}) || !strings(s, {"source_uri"})) return false;
        sc.id = s["id"].asInt(static_cast<int>(i));
        sc.sourceUri = s["source_uri"].asString("mock://stream" + std::to_string(i));

        const Json& regions = s["regions"];
        if (s.has("regions") && !regions.isArray()) { err = "regions must be an array"; return false; }
        if (regions.isArray()) {
            for (size_t r = 0; r < regions.size(); ++r) {
                RegionConfig rc;
                const Json& rg = regions[r];
                if (!rg.isObject()) { err = "region must be an object"; return false; }
                if (!integers(rg, {"id","cooldown_ms"})) return false;
                if (rg["cooldown_ms"].asInt(5000) < 0) { err = "cooldown_ms must be nonnegative"; return false; }
                rc.id = rg["id"].asInt(static_cast<int>(r));
                rc.cooldownMs = static_cast<unsigned>(rg["cooldown_ms"].asInt(5000));

                const Json& pts = rg["polygon"];
                if (!pts.isArray() || pts.size() < 3 || pts.size() > 256) { err = "polygon needs 3..256 vertices"; return false; }
                if (pts.isArray()) {
                    for (size_t p = 0; p < pts.size(); ++p) {
                        const Json& pt = pts[p];
                        if (!pt.isArray() || pt.size() != 2 || !pt[0].isNumber() || !pt[1].isNumber() ||
                            !std::isfinite(pt[0].asNumber()) || !std::isfinite(pt[1].asNumber()) ||
                            std::fabs(pt[0].asNumber()) > 1e6 || std::fabs(pt[1].asNumber()) > 1e6) {
                            err = "polygon vertex must be a finite numeric pair"; return false;
                        }
                        if (pt.isArray() && pt.size() >= 2) {
                            rc.polygon.push_back(Point{
                                static_cast<float>(pt[0].asNumber(0.0)),
                                static_cast<float>(pt[1].asNumber(0.0))});
                        }
                    }
                }
                if (rg.has("classes")) {
                    if (!rg["classes"].isArray()) { err = "classes must be an array"; return false; }
                    const Json& cls = rg["classes"];
                    for (size_t c = 0; c < cls.size(); ++c) {
                        const double d = cls[c].asNumber(-1);
                        if (!cls[c].isNumber() || !std::isfinite(d) || std::floor(d) != d || d < 0 || d >= (out.hardware ? kNumCocoClassNames : kNumClassNames)) {
                            err = out.hardware ? "hardware class id must be an integer 0..79" : "mock class id must be an integer 0..4"; return false;
                        }
                        rc.classes.push_back(cls[c].asInt(-1));
                    }
                }
                sc.regions.push_back(rc);
            }
        }
        out.streams.push_back(sc);
    }
    if (!out.validate(err)) return false;
    destination = std::move(out);
    return true;
}

} // namespace msad
