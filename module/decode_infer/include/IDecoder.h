/*
 * IDecoder.h — 视频解码器抽象基类
 *
 * 职责：从视频源（本地文件 / RTSP 流）解码出 YUV420 图像帧。
 *
 * 当前 x86 实现：MockDecoder（模拟桩，按帧率生成模拟帧，不访问真实文件/网络）；
 * 后续 RK3588 实现：RkMppDecoder（MPP 硬解 H.264/H.265，支持文件与 RTSP）。
 *
 * 约定：
 *   - open() 成功后才可调用 decodeFrame()；
 *   - decodeFrame() 返回 false 表示解码结束/失败，调用方应停止该路；
 *   - close() 幂等，可多次调用。
 */
#ifndef MODULE_IDECODER_H
#define MODULE_IDECODER_H

#include <string>

#include "msad/Types.h"   /* msad::Frame */

namespace msad {

class IDecoder {
public:
    virtual ~IDecoder() = default;

    /*
     * 打开视频源。
     * @param uri 视频源标识：本地文件路径 或 rtsp:// 地址
     *            （mock 实现仅记录该串，不真正访问）
     * @return true=成功
     */
    virtual bool open(const std::string& uri) = 0;

    /*
     * 解码出一帧。
     * @param frame 输出帧（YUV420 模拟图像数据）
     * @return true=取到一帧；false=流结束/出错
     */
    virtual bool decodeFrame(Frame& frame) = 0;

    /* 关闭并释放资源（幂等） */
    virtual void close() = 0;

    /* 描述信息（日志用） */
    virtual std::string name() const = 0;
};

} // namespace msad

#endif // MODULE_IDECODER_H
