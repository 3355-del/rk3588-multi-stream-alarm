#include "msad/VideoPipeline.h"

#include <chrono>
#include <filesystem>
#include <set>
#include <stdexcept>

#include "msad/Logger.h"
#include "msad/Nms.h"

namespace msad {

uint64_t VideoPipeline::nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/* 告警线程：消费检测结果 → 警戒区判断 → 冷却去重 → 截图 + TCP 上报 */
void VideoPipeline::alarmLoop(int idx) {
    Logger::info("Pipeline", "alarm thread start, stream=" + std::to_string(idx));
    while (running_.load()) {
        DetectResult res;
        if (!qInf2Alarm_[idx]->pop(res)) break;
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        if (!previewOnly_) assignTracks(idx, res, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
        if (observer_) observer_(res); // Before alarm handling can move the frame into a screenshot task.
        if (!previewOnly_) checkAndRaiseAlarm(idx, res);
    }
    Logger::info("Pipeline", "alarm thread exit, stream=" + std::to_string(idx));
}

void VideoPipeline::assignTracks(int idx, DetectResult& res, uint64_t now) {
    constexpr uint64_t maxGapMs = 1000;
    constexpr size_t maxTracks = 2048;
    auto& tracks = tracks_[idx];
    for (auto it = tracks.begin(); it != tracks.end();) {
        if (now - it->second.lastSeen > maxGapMs) it = tracks.erase(it);
        else ++it;
    }
    std::set<uint64_t> matched;
    for (auto& detection : res.detections) {
        auto best = tracks.end();
        float overlap = 0.25f;
        for (auto it = tracks.begin(); it != tracks.end(); ++it) {
            if (it->second.classId != detection.classId || matched.count(it->first)) continue;
            const float score = computeIoU(it->second.box, detection.box);
            if (score > overlap) {
                best = it;
                overlap = score;
            }
        }
        if (best == tracks.end()) {
            const uint64_t id = nextTrackId_.fetch_add(1);
            best = tracks.emplace(id, Track{detection.box, detection.classId, now, {}}).first;
        }
        best->second.box = detection.box;
        best->second.lastSeen = now;
        detection.trackId = best->first;
        matched.insert(best->first);
    }
    for (auto it = tracks.begin(); tracks.size() > maxTracks && it != tracks.end();) {
        if (!matched.count(it->first)) it = tracks.erase(it);
        else ++it;
    }
}

void VideoPipeline::checkAndRaiseAlarm(int idx, DetectResult& res) {
    /* 该路没有警戒区域 → 无告警逻辑 */
    if (regions_[idx].empty()) return;

    std::shared_ptr<const Frame> snapshot;
    const auto clock = std::chrono::steady_clock::now().time_since_epoch();
    const auto cooldownNow = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock).count());
    for (const auto& region : regions_[idx]) {
        for (const auto& det : res.detections) {
            /* 1. 类别过滤 */
            if (!region.caresAbout(det.classId)) continue;
            /* 2. 闯入判断：目标中心落入多边形 */
            if (!region.containsCenter(det.box)) continue;

            /* 3. 同一目标在同一区域单独冷却。 */
            auto& cooldown = tracks_[idx].at(det.trackId).lastAlarmByRegion;
            const auto previous = cooldown.find(region.id());
            if (previous != cooldown.end() && cooldownNow - previous->second < region.cooldownMs()) continue;

            /* 4. 触发告警：构造任务投递给 IO 线程（本线程不做磁盘/网络 IO，
             *    避免慢速写盘阻塞告警线程、进而反压整条流水线）；
             *    队列满时拒绝新任务且不更新冷却，下个检测帧可再次尝试。 */
            if (!snapshot) snapshot = std::make_shared<const Frame>(std::move(res.frame));
            const uint64_t now = nowMs();
            const uint64_t sequence = alarmSequence_.fetch_add(1);
            std::string name = "alarm_s" + std::to_string(cfg_.streams[idx].id) +
                "_r" + std::to_string(region.id()) + "_c" + std::to_string(det.classId) +
                "_" + std::to_string(now) + "_f" + std::to_string(snapshot->frameIndex) +
                "_a" + std::to_string(sequence) + "_" + instanceId_ + ".png";

            AlarmTask task;
            task.streamIndex = static_cast<size_t>(idx);
            task.frame = snapshot;                 /* 帧快照（截图用） */
            task.boxes.push_back(det);              /* 截图上画该目标框 */
            task.shotPath = cfg_.screenshotDir + "/" + name;
            task.info.eventId = instanceId_ + "-" + std::to_string(sequence);
            task.info.timestampMs = now;
            task.info.streamId = res.streamId;
            task.info.regionId = region.id();
            task.info.classId = det.classId;
            task.info.trackId = det.trackId;
            task.info.confidence = det.confidence;
            task.info.centerX = det.box.center().x;
            task.info.centerY = det.box.center().y;
            task.info.screenshotFile.clear();        /* 由 IO 线程写盘成功后回填 */

            if (!alarmTasks_) return;
            const auto result = alarmTasks_->tryPush(std::move(task));
            if (result == RingBuffer<AlarmTask>::PushResult::Stopped) return;
            if (result == RingBuffer<AlarmTask>::PushResult::Full) continue;
            cooldown[region.id()] = cooldownNow;
            {
                std::lock_guard<std::mutex> statsLock(statsMtx_);
                ++stats_[idx].alarms; // Accepted tasks only; rejected tasks do not enter cooldown.
            }
        }
    }
}

/*
 * IO 线程：串行消费告警任务 → 写截图 PNG → TCP 上报。
 * 与告警线程解耦：即使写盘/网络变慢，也只影响本线程与新任务的排队，
 * 不会阻塞警戒区判定与整条推理流水线。
 */
void VideoPipeline::ioLoop() {
    Logger::info("Pipeline", "io thread start (screenshot + tcp report)");
    while (true) {
        AlarmTask task;
        if (!alarmTasks_ || !alarmTasks_->pop(task)) break;

        /* 1. 写截图（失败则不上报文件名，避免接收端取到不存在的文件） */
        const auto freeBytes = std::filesystem::space(cfg_.screenshotDir).available;
        const uint64_t reserve = 256ULL*1024*1024 + static_cast<uint64_t>(cfg_.frameWidth)*static_cast<uint64_t>(cfg_.frameHeight)*4;
        if (freeBytes < reserve) throw std::runtime_error("screenshot disk space below reserve; stopping before disk fills");
        const bool saved = screenshotWriter_(*task.frame, task.shotPath, task.boxes);
        AlarmInfo info = task.info;
        info.screenshotFile = saved ? std::filesystem::path(task.shotPath).filename().string() : "";

        /* 2. TCP 上报（TcpAlarmNotifier 内部保证断线重连与发送互斥） */
        const bool sent = notifier_ ? notifier_->sendAlarm(info) : false;
        if (sent) {
            std::lock_guard<std::mutex> statsLock(statsMtx_);
            ++stats_[task.streamIndex].sent;
        }

        Logger::info("Pipeline",
                     "ALARM stream=" + std::to_string(info.streamId) +
                     " region=" + std::to_string(info.regionId) +
                     " class=" + std::string(cocoClassName(info.classId)) +
                     " conf=" + std::to_string(info.confidence) +
                     std::string(cfg_.alarm.reliable ? " persisted=" : " tcp_sent=") + (sent ? "yes" : "no") +
                     " shot=" + (saved ? task.shotPath : "FAILED"));
    }
    Logger::info("Pipeline", "io thread exit");
}

} // namespace msad
