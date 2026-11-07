# Wreck - Real-Time Cinematic Adaptive Grading Engine (Android / C)

[![Platform](https://img.shields.io/badge/Platform-Android%2015-brightgreen.svg)]()
[![Arch](https://img.shields.io/badge/Architecture-ARM64--v8a%20%7C%20NEON-blue.svg)]()
[![Hardware](https://img.shields.io/badge/Target-Snapdragon%208%20Elite%20%7C%20OnePlus%2013-orange.svg)]()
[![License](https://img.shields.io/badge/License-MIT-green.svg)]()

**Wreck** 是面向旗舰移动端（专为高通骁龙 8 至尊版 / OnePlus 13 深度调优）打造的超低时延、全端侧实时的自适应电影级调色相机与图像处理引擎。

核心算法完全采用纯 C 语言编写，深度整合 ARM64 NEON SIMD 矢量指令集与条件变量事件驱动多核切片并发，配合 Camera2 硬件 ISP 协同旁路设计，实现 **9.1ms / 110 FPS** 满帧实时取景，并支持 **12.5MP (4096x3072 / 6~8MB)** 高分辨率电影成片直接拍摄落盘至系统 DCIM 相册。

---

## 核心特性 (Key Features)

- **15 款殿堂级电影色彩美学风格**：港风复古、电影冷青、暗调情绪、复古暖阳、富士胶片、黑白银盐等 15 种风格解算器。
- **极致端侧实时性**：
  - 实时取景渲染延迟低至 **`9.11 ms`**（Snapdragon 8 Elite 真机实测）；
  - 取景器 **30.0 FPS** 锁满硬件传感器，理论算力吞吐达 **109.8 FPS**。
- **12.5MP 原生高画质大图捕获**：
  - 拍照产物采用 `4096x3072` 高动态原始传感器采样；
  - 产物 JPEG 体积达到 **6 ~ 8 MB**，具有原生相机的超清细节与微反差；
  - 自动通过 MediaStore Scoped Storage 保存至系统相册 `DCIM/AdaptiveGrading/`。
- **零 CPU 占用取景渲染**：
  - 基于 `android.graphics.Matrix` 的 90° 顺时针旋转物理校正与 Aspect-Fill（Center-Crop）等比例居中裁切；
  - 直接调用 `SurfaceHolder.lockHardwareCanvas()` 硬件画布交由 Adreno GPU 并行渲染，彻底根治形变与拉伸。
- **超轻量级架构**：
  - 打包生成独立的 `Wreck-v2.1.apk` 仅约 **900 KB**；
  - 零第三方庞大依赖，无需 OpenCV、无需 PyTorch，纯 C + NDK + Kotlin 原生轻量打造。

---

## 项目工程架构 (Directory Structure)

```
Wreck/
├── c_core/                 # 纯 C 核心调色库 (无平台依赖)
│   ├── include/            # 算法头文件 (自适应解算器、NEON、线程池、日志、存储)
│   └── src/                # 算法实现文件 (自适应调色管线、时域平滑、银盐暗角)
├── android/                # Android 宿主 App 与 SDK
│   ├── AndroidManifest.xml # 应用清单声明 (目标 Android 15 / API 35)
│   ├── java/               # 交互界面与流水线 (CameraActivity, CameraPreviewView)
│   ├── jni/                # JNI 桥接层与图形锁定绑定
│   └── adaptive-grading-sdk/# 独立可复用的 Android AAR 库模块
├── scripts/                # 构建流水线与自动化打包脚本
│   ├── build_and_install_apk.py # 一键免 IDE 编译打包与真机 ADB 自动部署
│   └── ci_pipeline.py      # 本地 CI 持续集成多阶段测试脚本
├── tests/                  # 单元测试与真机压力测试套件
└── tools/                  # 基准黄金测试向量验证脚本
```

---

## 编译与安装 (Build & Deployment)

本项目自带极简便携式工具链配置，无需安装重量级 Android Studio 即可完成全自动编译与签名：

```bash
# 执行一键跨平台编译、对齐、签名并安装至真机
python scripts/build_and_install_apk.py
```

安装完成后，手机桌面将显示正式命名的 **Wreck** 应用程序。

---

## 开源许可 (License)

本项目基于 [MIT 许可证](LICENSE) 开源。
