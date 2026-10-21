# RK3588 多路边缘目标检测告警系统

## 1. 项目定位

RK3588 上的多路视频边缘 AI 目标检测告警系统，板端完成解码/预处理/推理/告警，只上报事件不上传视频。C++17 实现，提供无硬件 mock 模式与板卡真实 HAL。

## 2. 架构与数据流

```text
USB(V4L2) / 本地视频、RTSP(FFmpeg 解复用 + MPP 硬解)
  → 采集 → 有界队列 → 预处理(RGA letterbox)
  → 有界队列 → 推理(RKNN INT8、DFL、NMS)
  → 有界队列 → 告警(多边形区域、冷却时间)
  → 独立 IO 线程：PNG 截图、本地待发队列、TCP JSON + ACK
  → Python 接收端：SQLite 提交后确认，按 event_id 去重
```

四阶段流水线加独立 IO 线程；队列满时丢最旧帧。MPP → RGA → RKNN 使用 DMA-BUF，USB 采集和截图仍有拷贝。使用 `rknn_dup_context` 复用模型上下文；并发门限和队列限制资源占用。截图留在本地，接收端保存事件及截图路径，不通过此协议传输视频。

`include/` 为接口，`src/hal/` 为真实与模拟后端，`src/pipeline/` 为流水线，`src/algo/` 为后处理，`tools/` 为接收端、验证及划区工具。`module/` 保留构建和回归依赖的模拟练习模块，不代表额外硬件实测能力。

## 3. 关键指标

以下为作者提供的历史板卡实测记录（Orange Pi 5 / RK3588S / 8 GB），不是本次整理重新测得，也不保证更换模型、素材后复现相同吞吐。原始影像和日志不公开。

| 项目 | 实测结果 |
| --- | --- |
| 单路同素材 20 秒分组对照 | CPU 预处理+浮点输出 25.39 FPS → DMA-BUF 输入+直读量化输出 35.05 FPS（+38%） |
| 取帧 / 解码耗时 | 4.9 → 1.33 ms；9.4 → 3.6 ms |
| 双路文件源 | 各约 29.9 FPS |
| 四路文件源 | 合计约 67.4 FPS |
| RTSP 回环 | 约 16.28 FPS |
| 摄像头 + 文件双路 | 连续 3600.7 秒无崩溃，115 条告警入库，两路丢帧 0 / 508 |
| 历史验证 | ASan 覆盖截图路径未报告内存错误（未启用泄漏检测）；CTest 4/4 通过 |
| DMA/CPU 输入与 INT8/float 检测等价性 | 12 帧，detections=89，PASS |

耗时术语说明：上表沿用原指标简称，“取帧”实际指获取推理输出，“解码”指检测输出后处理，不是摄像头取帧与 MPP 视频解码耗时。当前测试集比历史 4 项更多，本次验证结果另见仓库外检查报告。

模型来源边界：原项目 README 记录历史使用的已导出模型为 `agricultureiot/rknn-yolo` 的 YOLOv11n，输出布局兼容本项目的 YOLOv8 风格后处理。因此上述数据不标为自行转换 YOLOv8n 的实测成果。

## 4. 运行环境

历史验证环境：Orange Pi 5（RK3588S，8 GB）/ Ubuntu 22.04.5 / 内核 6.1.99 / RKNPU 驱动 0.9.8 / RKNN Runtime 2.3.0 / 自建 MPP（commit `14729dd578e570e5f00fd1dd2113f5429012d64b`）。

硬件模式需在板卡上构建，另装 RGA、MPP、FFmpeg 开发包及匹配的 RKNN SDK。仓库不携带 SDK 头文件、库或模型。无硬件模式需要 CMake ≥3.16、C++17 编译器、Python ≥3.10；Windows 可用 MinGW + Ninja。

## 5. 构建与复现

在仓库根执行以下命令。Windows 先将 CMake、Ninja、GCC 和 Python 加入 PATH；Linux 可去掉 `-G Ninja` 使用默认生成器。

```sh
cmake -S . -B build -G Ninja -DMSAD_ENABLE_RKNN=OFF -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

终端一启动接收端（保持运行）：

```sh
python tools/alarm_receiver.py --host 127.0.0.1 --port 9000 --db data/alarms.sqlite3
```

终端二运行 5 秒模拟演示：

```sh
./build/multi_stream_alarm_demo config/demo_config.json
```

Windows 可使用 `./build/multi_stream_alarm_demo.exe`。该模式生成模拟检测、截图和告警，用于验证工程链路，不证明真实模型精度。

Windows 若测试报告临时目录 `Permission denied`，可在当前 PowerShell 将测试临时目录改到工作区，再执行 CTest（仅影响当前终端，不修改系统权限）：

```powershell
New-Item -ItemType Directory -Force build/test-temp | Out-Null
$env:TEMP = (Resolve-Path build/test-temp).Path
$env:TMP = $env:TEMP
ctest --test-dir build --output-on-failure
```

如机器有多个 Python，可配置 `-DPython3_EXECUTABLE=YOUR_PYTHON_EXECUTABLE` 明确选择 Python 3.10 以上解释器。

板卡依赖安装好后，指定外部 SDK 的实际位置；以下占位参数必须替换：

```sh
cmake -S . -B build-board -DMSAD_ENABLE_RKNN=ON -DBUILD_TESTING=ON \
  -DMSAD_RKNN_INCLUDE_DIR=YOUR_RKNN_INCLUDE_DIR \
  -DMSAD_RKNN_LIBRARY=YOUR_LIBRKNNRT_FILE \
  -DMSAD_MPP_ROOT=YOUR_MPP_INSTALL_PREFIX
cmake --build build-board --parallel 2
ctest --test-dir build-board --output-on-failure
./build-board/multi_stream_alarm_demo config/rk3588_config.json
```

启动前自行准备 `model/model.rknn`，按设备调整配置中的视频源、旋转、类别及区域。SDK 动态库必须能被系统加载器找到。需要与其他 NPU 程序共存时遵守会话锁；本仓库不能保证不同运行时并行安全。

### 自行转换 YOLOv8n → INT8 RKNN

本仓库不提供权重，也未在本次整理执行转换。请在独立工作目录按官方工具的版本要求准备环境：

1. 获取合法使用的 YOLOv8n 权重；采用 [Rockchip 适配导出说明](https://github.com/airockchip/ultralytics_yolov8/blob/main/RKOPT_README.zh-CN.md)，配置 `ultralytics/cfg/default.yaml` 的模型路径，在该仓库运行 `PYTHONPATH=./ python ultralytics/engine/exporter.py` 导出 ONNX。普通融合输出的 ONNX 不保证兼容本工程。
2. 安装与目标 Runtime 兼容的 [RKNN-Toolkit2](https://github.com/airockchip/rknn-toolkit2)，保留所用版本和提交号；历史 Runtime 为 2.3.0，不应盲用最新转换产物。
3. 按 [Model Zoo YOLOv8 指引](https://github.com/airockchip/rknn_model_zoo/tree/main/examples/yolov8) 准备代表实际场景的校准图片清单。在其 `examples/yolov8/python/convert.py` 中将 `DATASET_PATH` 指向清单，然后执行 `python convert.py YOUR_YOLOV8N.onnx rk3588 i8 YOUR_OUTPUT.rknn`。
4. 使用 RGB、mean=0、std=255 的转换配置；板端输入原始 uint8，不重复归一化。核对输入尺寸和输出头布局、类别数、scale/zp。当前后处理以 COCO 80 类、三尺度检测头为目标；定制模型需适配。
5. 将产物放入忽略的 `model/` 目录，先跑硬件测试与等价性测试，再用带标注数据评价精度。没有标注不能声称完成 mAP 验证。

交互划区：运行 `python tools/region_editor.py`，打开终端显示的本机地址。默认使用独立离线配置副本；可载入自己有权使用的图片辅助划区。保存后需让检测程序使用该配置重新启动，不支持运行中热更新。板卡取景必须显式加 `--allow-capture`；电脑浏览器不代表自动调用电脑摄像头。

## 6. 已知边界

- USB 摄像头实测约 10 FPS 属该设备/链路限制（S_PARM 调节无效），非该次测试的流水线瓶颈。
- RTSP 仅用本机回环流验证，未接真实 IP 摄像机。
- 四路并发数据来自文件源，非四路物理摄像头。
- 未做模型量化转换（历史使用已导出的 INT8 模型）；上面的步骤为复现指引。
- 未评估 mAP/精度（无标注数据），只做了动态一致性与等价性验证。
- DMA-BUF 不是整条链路完全零拷贝；内存预算不是进程硬限制。ACK/持久化队列受磁盘容量和故障影响，不承诺无限缓存或绝对不丢失。
- 本次展示版在 Windows 验证模拟构建，不替代修改外部 SDK 引用后的板卡编译及实测。

## 许可

作者自有代码采用 MIT，见 [LICENSE](LICENSE)。第三方 SDK、模型、转换工具遵循各自许可；MIT 不授予模型或厂商组件的使用、分发权。见 [依赖说明](docs/dependencies.md)。
