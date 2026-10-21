# 外部依赖与公开范围

本展示仓库只包含作者自有源码、测试和文档。Rockchip 的 `rknn_api.h` 原文件含限制分发声明，未纳入本仓库或 Git 历史。请从有权使用的 SDK 获取，并通过 CMake 参数指定。

硬件依赖包括 RKNN Runtime、RGA、MPP、FFmpeg；Python 接收器使用标准库 SQLite。各依赖须遵循其原许可，不能因为本仓库采用 MIT 就将它们重新许可。

YOLO 权重、ONNX、RKNN、校准图片、视频、真实截图、日志与设备配置不分发。模型转换指导链接见 README，使用者自行核对模型和工具许可。

`module/decode_infer` 与 Linux 下的 `module/voice_mq` 是原 CMake/回归测试依赖，保留源码以确保干净克隆后构建完整。测试生成的数据仅供测试，不是历史板卡测量证据。
