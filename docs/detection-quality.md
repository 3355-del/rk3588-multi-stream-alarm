# 检测质量排查与回归

## 2026-10-30 已验证的范围

在板卡隔离部署中补充了180°路径验证。相同的文件素材分别以正向和180°运行，每种方向检查12帧：

- CPU与DMA预处理的图像平均字节误差小于2；
- 180°的CPU输出与正向输出的逐像素反转完全一致；
- 同一像素输入下，DMA/CPU推理输入以及INT8/float输出的类别、候选数量、置信度与框坐标通过既有容差检查；
- 正向候选总计89，旋转候选总计68，均非零，内存生命周期及PNG读取检查通过。

两个方向的输入内容朝向不同，候选数不可当作精度高低结论。测试素材是已有文件源，不是新采集的现场摄像头评测，也不是有标注数据集。原用户工程源码和配置未覆盖。

已核对模型输入为640×640 RGB、80类与三尺度DFL16布局。实现使用黑边并忽略score_sum，与[Rockchip官方YOLOv8 Python参考](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov8/python/yolov8.py)的对应约定一致。参考代码可能更新，应结合所用模型与SDK版本复核。当前未定位可证明导致现场漏检的软件错误。

## 复现路径一致性测试

在板卡按README完成硬件构建后，在独立输出目录运行，避免测试PNG覆盖已有文件。替换仓库及模型、视频的路径：

```sh
REPO_ROOT="$PWD"
MODEL=/absolute/path/to/model.rknn
VIDEO=/absolute/path/to/test.mp4
mkdir -p outputs/quality-normal outputs/quality-rotated
(cd outputs/quality-normal && "$REPO_ROOT/build-board/hardware_tests" "$MODEL" "$VIDEO")
(cd outputs/quality-rotated && "$REPO_ROOT/build-board/hardware_tests" "$MODEL" "$VIDEO" --flip180)
```

这里比较的是执行路径，所有路径一致也可能共同输出错类或漏检，不能代替精度评测。新测试没有降低检测阈值、合并错类或保留历史检测框。

## 仍需完成的精度验证

1. 收集目标应用中有代表性的普通物品、空场、不同尺寸、光照和遮挡样本，先明确要识别哪些类别。素材和标注保留私有。
2. 独立核对每张样本中的全部目标，不能把预测框直接当作标注。已有motion_eval可导出待审查标注模板；未审查标注会被评分工具拒绝。
3. 使用相同640×640 letterbox坐标与相同帧集合，运行tools/score_detections.py，统计固定阈值下各类precision/recall与TP/FP/FN。当前工具不计算mAP。
4. 对照同一素材后再决定成像、参数或模型调整，保留评测集，不用调参素材冒充独立验收集。

本次只补足旋转路径回归并完成初步约定核对，现场漏检和类别波动仍是未解决问题。没有标注数据时，不报告准确率提升，也不承诺换模型即可解决。
