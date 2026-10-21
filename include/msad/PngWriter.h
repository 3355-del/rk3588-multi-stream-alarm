/*
 * PngWriter.h — 告警截图保存（纯手写 PNG 编码器）
 *
 * 职责：
 *   - 将帧转换为 RGB888（自动识别 YUV420P / YUYV422 / RGB888 三种格式）；
 *   - 在图像上绘制检测框与类别标签（模拟屏幕叠加）；
 *   - 用自研 PNG 编码器（无第三方依赖，采用 zlib "stored" 块策略）
 *     写入磁盘，供告警留存取证。
 *
 * 格式识别规则（按 data 字节数推断，与 Frame 不携带格式字段的现状一致）：
 *   size == w*h*3   → RGB888（RK3588 上 RGA 预处理输出，直接使用）
 *   size == w*h*2   → YUYV422（USB 摄像头原始帧）
 *   其他（w*h*3/2） → YUV420P（模拟解码帧）
 *
 * 移植说明：RK3588 上该模块仍可复用（截图保存与硬件无关）；
 * 若希望 RGA 直接出图，可在 IFrameProcessor 中增加导出接口。
 */
#ifndef MULTI_STREAM_PNGWRITER_H
#define MULTI_STREAM_PNGWRITER_H

#include <string>
#include <vector>

#include "msad/Types.h"

namespace msad {

class PngWriter {
public:
    /*
     * 保存一帧画面为 PNG（自动识别 YUV420P / YUYV422 / RGB888）。
     * @param frame       源帧
     * @param path        输出 PNG 路径（父目录需已存在）
     * @param detections  需要叠加绘制的目标框（可空）
     * @return true=成功
     */
    static bool saveFrameAsPng(const Frame& frame,
                               const std::string& path,
                               const std::vector<Detection>& detections);

private:
    /* YUV420 → RGB888（BT.601 范围） */
    static void yuv420ToRgb(const Frame& frame, std::vector<uint8_t>& rgb);

    /* YUYV422 → RGB888（JFIF/全范围，USB 摄像头原始格式） */
    static void yuyvToRgb(const Frame& frame, std::vector<uint8_t>& rgb);

    /* 在 RGB 图像上绘制矩形框与类别文本占位（简化字体：色块） */
    static void drawBoxes(int width, int height,
                          std::vector<uint8_t>& rgb,
                          const std::vector<Detection>& detections);

    /* 手写 PNG 编码：RGB888 → PNG 字节流 */
    static bool encodePng(int width, int height,
                          const std::vector<uint8_t>& rgb,
                          std::vector<uint8_t>& out);

    /* zlib 数据构造（stored/deflate 无压缩块 + adler32） */
    static void buildZlib(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out);

    /* CRC32（PNG 块校验） */
    static uint32_t crc32(const uint8_t* data, size_t len);
    /* Adler-32 */
    static uint32_t adler32(const uint8_t* data, size_t len);
    /* 大端写 32 位 */
    static void writeBe32(std::vector<uint8_t>& out, uint32_t v);
};

} // namespace msad

#endif // MULTI_STREAM_PNGWRITER_H
