/*
 * PngWriter.cpp — 手写 PNG 编码器实现
 *
 * PNG 结构：
 *   8 字节签名 + [长度(4) + 类型(4) + 数据 + CRC(4)] 若干块
 *   必备块：IHDR / IDAT / IEND
 *   IDAT 内容为 zlib 流（此处采用 deflate stored 块 + adler32，
 *   无压缩但实现简单、依赖为零，适合截图场景）。
 */
#include "msad/PngWriter.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>

#include "msad/Logger.h"

namespace msad {

static const uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47,
                                         0x0D, 0x0A, 0x1A, 0x0A};

/* ---------- YUV420 → RGB888 ---------- */

void PngWriter::yuv420ToRgb(const Frame& frame, std::vector<uint8_t>& rgb) {
    const int w = frame.width;
    const int h = frame.height;
    if (w <= 0 || h <= 0) return;

    const size_t yPlane = static_cast<size_t>(w) * h;
    const size_t uPlane = yPlane / 4;
    rgb.assign(static_cast<size_t>(w) * h * 3, 0);

    const uint8_t* Y = frame.data.data();
    const uint8_t* U = Y + yPlane;
    const uint8_t* V = Y + yPlane + uPlane;

    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            uint8_t yVal = Y[yy * w + xx];
            /* UV 每 2x2 共享一份，取块左上角 */
            uint8_t uVal = U[(yy / 2) * (w / 2) + (xx / 2)];
            uint8_t vVal = V[(yy / 2) * (w / 2) + (xx / 2)];

            /* BT.601 逆变换 */
            float c = static_cast<float>(yVal) - 16.0f;
            float d = static_cast<float>(uVal) - 128.0f;
            float e = static_cast<float>(vVal) - 128.0f;

            int r = static_cast<int>(1.164f * c + 1.596f * e);
            int g = static_cast<int>(1.164f * c - 0.392f * d - 0.813f * e);
            int b = static_cast<int>(1.164f * c + 2.017f * d);

            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);

            size_t idx = (static_cast<size_t>(yy) * w + xx) * 3;
            rgb[idx + 0] = static_cast<uint8_t>(r);
            rgb[idx + 1] = static_cast<uint8_t>(g);
            rgb[idx + 2] = static_cast<uint8_t>(b);
        }
    }
}

/* ---------- YUYV422 → RGB888 ---------- */

/*
 * USB 摄像头 YUYV 打包格式：每 4 字节 [Y0 U Y1 V] 表示 2 个像素。
 * 采用 JFIF（全范围）系数，与 UVC 摄像头输出范围一致。
 */
void PngWriter::yuyvToRgb(const Frame& frame, std::vector<uint8_t>& rgb) {
    const int w = frame.width;
    const int h = frame.height;
    if (w <= 0 || h <= 0) return;

    rgb.assign(static_cast<size_t>(w) * h * 3, 0);
    const uint8_t* src = frame.data.data();

    for (int yy = 0; yy < h; ++yy) {
        const uint8_t* row = src + static_cast<size_t>(yy) * w * 2;
        for (int xx = 0; xx + 1 < w; xx += 2) {
            const int y0 = row[xx * 2 + 0];
            const int u  = row[xx * 2 + 1];
            const int y1 = row[xx * 2 + 2];
            const int v  = row[xx * 2 + 3];

            const float du = static_cast<float>(u) - 128.0f;
            const float dv = static_cast<float>(v) - 128.0f;
            const int rAdd = static_cast<int>(1.402f * dv);
            const int gAdd = static_cast<int>(-0.344136f * du - 0.714136f * dv);
            const int bAdd = static_cast<int>(1.772f * du);

            for (int k = 0; k < 2; ++k) {
                const int yv = (k == 0) ? y0 : y1;
                int r = yv + rAdd, g = yv + gAdd, b = yv + bAdd;
                r = r < 0 ? 0 : (r > 255 ? 255 : r);
                g = g < 0 ? 0 : (g > 255 ? 255 : g);
                b = b < 0 ? 0 : (b > 255 ? 255 : b);
                const size_t idx = (static_cast<size_t>(yy) * w + (xx + k)) * 3;
                rgb[idx + 0] = static_cast<uint8_t>(r);
                rgb[idx + 1] = static_cast<uint8_t>(g);
                rgb[idx + 2] = static_cast<uint8_t>(b);
            }
        }
    }
}

/* ---------- 绘制检测框 ---------- */

void PngWriter::drawBoxes(int width, int height, std::vector<uint8_t>& rgb,
                          const std::vector<Detection>& detections) {
    /* 框线颜色（红色）与标签底色 */
    const uint8_t kBoxR = 255, kBoxG = 0, kBoxB = 0;
    const int kLineW = 2;

    for (const auto& d : detections) {
        if (!std::isfinite(d.box.x) || !std::isfinite(d.box.y) ||
            !std::isfinite(d.box.w) || !std::isfinite(d.box.h) ||
            d.box.w <= 0 || d.box.h <= 0) continue;
        if (d.box.x >= static_cast<float>(width) || d.box.y >= static_cast<float>(height) ||
            d.box.x + d.box.w < 0 || d.box.y + d.box.h < 0) continue;
        int x0 = static_cast<int>(std::clamp(d.box.x, 0.0f, static_cast<float>(width - 1)));
        int y0 = static_cast<int>(std::clamp(d.box.y, 0.0f, static_cast<float>(height - 1)));
        int x1 = static_cast<int>(std::clamp(d.box.x + d.box.w, 0.0f, static_cast<float>(width - 1)));
        int y1 = static_cast<int>(std::clamp(d.box.y + d.box.h, 0.0f, static_cast<float>(height - 1)));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= width)  x1 = width - 1;
        if (y1 >= height) y1 = height - 1;

        /* 上下两条水平线 */
        for (int xx = x0; xx <= x1; ++xx) {
            for (int k = 0; k < kLineW; ++k) {
                int yTop = y0 + k, yBot = y1 - k;
                auto setPx = [&](int px, int py) {
                    size_t idx = (static_cast<size_t>(py) * width + px) * 3;
                    rgb[idx + 0] = kBoxR; rgb[idx + 1] = kBoxG; rgb[idx + 2] = kBoxB;
                };
                if (yTop >= 0 && yTop < height) setPx(xx, yTop);
                if (yBot >= 0 && yBot < height) setPx(xx, yBot);
            }
        }
        /* 左右两条竖线 */
        for (int yy = y0; yy <= y1; ++yy) {
            for (int k = 0; k < kLineW; ++k) {
                int xL = x0 + k, xR = x1 - k;
                auto setPx = [&](int px, int py) {
                    size_t idx = (static_cast<size_t>(py) * width + px) * 3;
                    rgb[idx + 0] = kBoxR; rgb[idx + 1] = kBoxG; rgb[idx + 2] = kBoxB;
                };
                if (xL >= 0 && xL < width) setPx(xL, yy);
                if (xR >= 0 && xR < width) setPx(xR, yy);
            }
        }

        /* 顶部标签色块：模拟 "person 0.93" 文本（简化像素字体可后续扩展） */
        int labelW = 40, labelH = 10;
        if (x0 + labelW < width && y0 - labelH >= 0) {
            for (int yy = y0 - labelH; yy < y0; ++yy) {
                for (int xx = x0; xx < x0 + labelW && xx < width; ++xx) {
                    size_t idx = (static_cast<size_t>(yy) * width + xx) * 3;
                    rgb[idx + 0] = 255; rgb[idx + 1] = 255; rgb[idx + 2] = 255;
                }
            }
        }
    }
}

/* ---------- CRC32 ---------- */

uint32_t PngWriter::crc32(const uint8_t* data, size_t len) {
    /* 标准 CRC-32（PNG 用多项式 0xEDB88320，初始 0xFFFFFFFF，结果取反） */
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1)));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ---------- Adler-32 ---------- */

uint32_t PngWriter::adler32(const uint8_t* data, size_t len) {
    const uint32_t kMod = 65521u;
    uint32_t a = 1u, b = 0u;
    for (size_t i = 0; i < len; ++i) {
        a += data[i];
        b += a;
        /* 周期性取模防溢出 */
        if (i % 5552 == 5551) { a %= kMod; b %= kMod; }
    }
    a %= kMod;
    b %= kMod;
    return (b << 16) | a;
}

/* ---------- 工具 ---------- */

void PngWriter::writeBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

/*
 * 构造 zlib 流：2 字节头 + deflate stored 块 + adler32。
 * stored 块格式：BFINAL+BTYPE(1 字节) + LEN(2) + NLEN(2) + 原始数据
 */
void PngWriter::buildZlib(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(raw.size() + 5 * std::max<size_t>(1, (raw.size()+65534)/65535) + 6);
    /* zlib 头：CMF=0x78, FLG=0x01（无压缩，无字典） */
    out.push_back(0x78);
    out.push_back(0x01);

    /* deflate stored 块（BFINAL=1, BTYPE=00 → 0x01） */
    size_t offset = 0;
    do {
        const auto len = static_cast<uint16_t>(std::min<size_t>(65535, raw.size() - offset));
        out.push_back(offset + len == raw.size() ? 0x01 : 0x00);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>(len >> 8));
        const auto nlen = static_cast<uint16_t>(~len);
        out.push_back(static_cast<uint8_t>(nlen & 0xFF));
        out.push_back(static_cast<uint8_t>(nlen >> 8));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                   raw.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    } while (offset < raw.size());

    /* adler32（大端） */
    uint32_t ad = adler32(raw.data(), raw.size());
    out.push_back(static_cast<uint8_t>((ad >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((ad >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((ad >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(ad & 0xFF));
}

bool PngWriter::encodePng(int width, int height,
                          const std::vector<uint8_t>& rgb,
                          std::vector<uint8_t>& out) {
    out.clear();
    const size_t rawBytes = (static_cast<size_t>(width)*3+1)*static_cast<size_t>(height);
    out.reserve(rawBytes + 5*((rawBytes+65534)/65535) + 6 + 57);
    out.insert(out.end(), kPngSignature, kPngSignature + 8);

    /* IHDR：宽、高、位深 8、颜色类型 2(RGB)、压缩/滤波/隔行 = 0 */
    {
        std::vector<uint8_t> data;
        writeBe32(data, static_cast<uint32_t>(width));
        writeBe32(data, static_cast<uint32_t>(height));
        data.push_back(8);   /* bit depth */
        data.push_back(2);   /* color type: truecolor RGB */
        data.push_back(0);   /* compression */
        data.push_back(0);   /* filter */
        data.push_back(0);   /* interlace */

        writeBe32(out, static_cast<uint32_t>(data.size()));
        out.insert(out.end(), {'I', 'H', 'D', 'R'});
        out.insert(out.end(), data.begin(), data.end());
        uint32_t crc = crc32(out.data() + out.size() - data.size() - 4, data.size() + 4);
        writeBe32(out, crc);
    }

    /* IDAT：每行前置滤波类型 0（None），组成扫描线原始数据 */
    {
        std::vector<uint8_t> raw;
        raw.reserve((static_cast<size_t>(width) * 3 + 1) * height);
        for (int yy = 0; yy < height; ++yy) {
            raw.push_back(0);   /* filter type: None */
            size_t rowStart = static_cast<size_t>(yy) * width * 3;
            raw.insert(raw.end(), rgb.begin() + static_cast<long>(rowStart),
                       rgb.begin() + static_cast<long>(rowStart) + static_cast<size_t>(width) * 3);
        }

        std::vector<uint8_t> zlibData;
        buildZlib(raw, zlibData);

        writeBe32(out, static_cast<uint32_t>(zlibData.size()));
        out.insert(out.end(), {'I', 'D', 'A', 'T'});
        out.insert(out.end(), zlibData.begin(), zlibData.end());
        uint32_t crc = crc32(out.data() + out.size() - zlibData.size() - 4,
                             zlibData.size() + 4);
        writeBe32(out, crc);
    }

    /* IEND */
    {
        writeBe32(out, 0);
        out.insert(out.end(), {'I', 'E', 'N', 'D'});
        uint32_t crc = crc32(out.data() + out.size() - 4, 4);
        writeBe32(out, crc);
    }
    return true;
}

bool PngWriter::saveFrameAsPng(const Frame& frame, const std::string& path,
                               const std::vector<Detection>& detections) {
    if (!frame.validDimensions()) {
        Logger::error("PngWriter", "invalid frame dimensions");
        return false;
    }

    /* 1. 按 data 字节数识别帧格式（Frame 不携带格式字段）→ RGB888 */
    const size_t pixels = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    std::vector<uint8_t> rgb;
    if(frame.copyRgb){
        if(!frame.copyRgb(rgb)||rgb.size()!=pixels*3)return false;
    } else if (frame.data.size() == pixels * 2) {
        yuyvToRgb(frame, rgb);                  /* YUYV422（USB 摄像头原始帧） */
    } else if (frame.data.size() == pixels * 3) {
        rgb = frame.data;                       /* RGB888（RGA 预处理输出，直接使用） */
    } else if (frame.data.size() >= frame.yuvSize()) {
        yuv420ToRgb(frame, rgb);                /* YUV420P（模拟解码帧） */
    } else {
        Logger::error("PngWriter", "frame data size " + std::to_string(frame.data.size()) +
                                   " does not match any supported format");
        return false;
    }
    if (rgb.empty()) return false;

    /* 2. 叠加检测框 */
    drawBoxes(frame.width, frame.height, rgb, detections);

    /* 3. PNG 编码 */
    std::vector<uint8_t> png;
    if (!encodePng(frame.width, frame.height, rgb, png)) return false;

    /* 4. 写盘 */
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        Logger::error("PngWriter", "cannot open file: " + path);
        return false;
    }
    size_t written = std::fwrite(png.data(), 1, png.size(), fp);
    const int closeResult = std::fclose(fp);

    if (written != png.size() || closeResult != 0) {
        Logger::error("PngWriter", "short write: " + path);
        return false;
    }
    Logger::info("PngWriter", "saved screenshot: " + path +
                 " (" + std::to_string(png.size()) + " bytes)");
    return true;
}

} // namespace msad
