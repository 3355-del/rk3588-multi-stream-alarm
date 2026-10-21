/*
 * RealVideoSource.cpp — V4L2 USB 摄像头采集（IVideoSource 的 RK3588 实现）
 *
 * 采集格式：YUYV 422（免 MJPEG 解码，RGA 可直接做格式转换 + 缩放）。
 * 缓冲策略：V4L2 MMAP 4 缓冲区 + select 超时等待，取出即拷贝一帧后立刻归还，
 *           保证驱动侧缓冲始终在队列中、采集不中断。
 *
 * 线程模型：由流水线采集线程独占调用（open → readFrame* → close），非线程安全。
 */
#include "msad/RealComponents.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "msad/Logger.h"

namespace msad {

namespace {

/* 单次 select 等待时长；总等待上限 = kSelectTimeoutMs * kGrabRetries */
constexpr int kSelectTimeoutMs = 200;
constexpr int kGrabRetries = 5;      /* 最多约 1 秒无帧则判定为流中断 */

uint64_t epochMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000);
}

} // namespace

V4L2CameraSource::V4L2CameraSource(int width, int height) : width_(width), height_(height) {}

V4L2CameraSource::~V4L2CameraSource() {
    close();
}

bool V4L2CameraSource::open(const std::string& uri) {
    close();
    device_ = uri.empty() ? std::string("/dev/video0") : uri;

    fd_ = ::open(device_.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        Logger::error("V4L2", "cannot open " + device_ + ": " + std::strerror(errno));
        return false;
    }

    struct v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        Logger::error("V4L2", "VIDIOC_QUERYCAP failed: " + std::string(std::strerror(errno)));
        close();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        Logger::error("V4L2", device_ + " is not a video capture device");
        close();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        Logger::error("V4L2", device_ + " does not support streaming I/O");
        close();
        return false;
    }

    /* 请求 YUYV 422（免解码）；驱动会回填实际生效的尺寸 */
    struct v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<uint32_t>(width_);
    fmt.fmt.pix.height = static_cast<uint32_t>(height_);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        Logger::error("V4L2", "VIDIOC_S_FMT(YUYV) failed: " + std::string(std::strerror(errno)));
        close();
        return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        Logger::error("V4L2", device_ + " does not support YUYV 422");
        close();
        return false;
    }
    width_ = static_cast<int>(fmt.fmt.pix.width);
    height_ = static_cast<int>(fmt.fmt.pix.height);
    Frame dimensions;dimensions.width=width_;dimensions.height=height_;
    rowStride_=fmt.fmt.pix.bytesperline;
    if(!dimensions.validDimensions()||rowStride_<static_cast<size_t>(width_)*2||rowStride_>32768){
        Logger::error("V4L2","invalid driver dimensions/row stride");close();return false;
    }

    /* 申请 4 个 MMAP 缓冲 */
    struct v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2 || req.count > 32) {
        Logger::error("V4L2", "VIDIOC_REQBUFS failed: " + std::string(std::strerror(errno)));
        close();
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            Logger::error("V4L2", "VIDIOC_QUERYBUF failed: " + std::string(std::strerror(errno)));
            close();
            return false;
        }
        buffers_[i].length = buf.length;
        buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) {
            buffers_[i].start = nullptr;
            Logger::error("V4L2", "mmap failed: " + std::string(std::strerror(errno)));
            close();
            return false;
        }
    }

    /* 全部入队并开启流 */
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            Logger::error("V4L2", "VIDIOC_QBUF failed: " + std::string(std::strerror(errno)));
            close();
            return false;
        }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        Logger::error("V4L2", "VIDIOC_STREAMON failed: " + std::string(std::strerror(errno)));
        close();
        return false;
    }
    streaming_ = true;
    Logger::info("V4L2", "camera opened: " + device_ + " " + std::to_string(width_) + "x" +
                             std::to_string(height_) + " YUYV422, buffers=" + std::to_string(req.count));
    return true;
}

bool V4L2CameraSource::readFrame(Frame& frame) {
    if (fd_ < 0 || !streaming_) return false;

    for (int attempt = 0; attempt < kGrabRetries; ++attempt) {
        /* 等待帧就绪（带重试，避免瞬时抖动直接终止整条流水线） */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        struct timeval tv;
        tv.tv_sec = kSelectTimeoutMs / 1000;
        tv.tv_usec = (kSelectTimeoutMs % 1000) * 1000;
        const int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            Logger::error("V4L2", "select failed: " + std::string(std::strerror(errno)));
            return false;
        }
        if (r == 0) continue;                   /* 本轮超时，再等一轮 */

        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;   /* 缓冲尚未就绪，继续等 */
            Logger::error("V4L2", "VIDIOC_DQBUF failed: " + std::string(std::strerror(errno)));
            return false;
        }
        if (buf.index >= buffers_.size() || !buffers_[buf.index].start) {
            Logger::error("V4L2", "invalid buffer index from driver");
            return false;
        }

        /* 拷贝一帧（YUYV：2 字节/像素），随即归还缓冲以免采集停摆 */
        const size_t expected = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
        const size_t available=std::min(static_cast<size_t>(buf.bytesused),buffers_[buf.index].length);
        const size_t needed=(static_cast<size_t>(height_)-1)*rowStride_+static_cast<size_t>(width_)*2;
        if(available<needed||(buf.flags&V4L2_BUF_FLAG_ERROR)){
            ioctl(fd_,VIDIOC_QBUF,&buf);Logger::error("V4L2","short/corrupt capture buffer");return false;
        }
        const uint8_t* src=static_cast<const uint8_t*>(buffers_[buf.index].start);
        frame=Frame{};frame.data.resize(expected);
        for(int row=0;row<height_;++row)std::memcpy(frame.data.data()+static_cast<size_t>(row)*static_cast<size_t>(width_)*2,
            src+static_cast<size_t>(row)*rowStride_,static_cast<size_t>(width_)*2);
        frame.pixelFormat=Frame::PixelFormat::YUYV422;

        lastIndex_ = buf.index;
        struct v4l2_buffer qbuf;
        std::memset(&qbuf, 0, sizeof(qbuf));
        qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        qbuf.memory = V4L2_MEMORY_MMAP;
        qbuf.index = lastIndex_;
        if(ioctl(fd_, VIDIOC_QBUF, &qbuf)<0){Logger::error("V4L2","capture requeue failed");return false;}

        frame.width = width_;
        frame.height = height_;
        frame.timestampMs = epochMs();
        frame.frameIndex = frameIndex_++;
        return true;
    }

    Logger::error("V4L2", "no frame within timeout, camera stalled?");
    return false;
}

void V4L2CameraSource::close() {
    if (fd_ < 0) {
        buffers_.clear();
        return;
    }
    if (streaming_) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
    }
    buffers_.clear();
    ::close(fd_);
    fd_ = -1;
}

} // namespace msad
