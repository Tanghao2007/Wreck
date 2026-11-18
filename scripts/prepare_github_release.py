#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Prepare Clean GitHub Release Directory
======================================
Copies all clean source code, build scripts, tests, headers, and docs
to github_release/ directory, excluding binaries, caches, and APKs.
"""

import os
import shutil
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TARGET_DIR = PROJECT_ROOT / "github_release"

EXCLUDE_DIRS = {
    ".git", ".gradle", ".cache", ".idea", ".vscode",
    "build", "apk_staging", "__pycache__", "tools",
    "github_release", ".system_generated",
    "c_outputs", "real_device_c_outputs", "real_device_outputs",
    "android-sdk", "jdk-17", "kotlinc", "platform-tools",
    "zig-windows-x86_64-0.13.0"
}

EXCLUDE_EXTS = {
    ".apk", ".idsig", ".so", ".dll", ".exe", ".o", ".a",
    ".class", ".dex", ".pyc", ".idx", ".lock", ".bin", ".raw",
    ".jar", ".zip", ".tar", ".gz"
}

def copy_clean_tree(src_dir: Path, dst_dir: Path):
    dst_dir.mkdir(parents=True, exist_ok=True)
    for root, dirs, files in os.walk(src_dir):
        # Prune excluded directories
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS and not d.startswith(".")]

        rel_path = Path(root).relative_to(src_dir)
        target_root = dst_dir / rel_path
        target_root.mkdir(parents=True, exist_ok=True)

        for f in files:
            p = Path(f)
            if p.suffix.lower() in EXCLUDE_EXTS:
                continue
            if f.startswith("."):
                continue
            src_file = Path(root) / f
            dst_file = target_root / f
            shutil.copy2(src_file, dst_file)

def clear_dir(d: Path):
    if not d.exists():
        return
    for item in d.iterdir():
        if item.name == ".git":
            continue
        if item.is_dir():
            shutil.rmtree(item, ignore_errors=True)
        else:
            try:
                item.unlink()
            except Exception:
                pass

def main():
    print(f"Creating clean GitHub Release directory at: {TARGET_DIR}")
    clear_dir(TARGET_DIR)
    TARGET_DIR.mkdir(parents=True, exist_ok=True)

    # 1. Copy source trees
    for folder in ["c_core", "android", "scripts", "tests", "tools", "resources", "core"]:
            copy_clean_tree(src, TARGET_DIR / folder)

    # 2. Copy root documents
    for doc in [
        "README.md",
        "RELEASE_NOTES_v2.2.md",
        "RELEASE_NOTES_v2.1.md",
        "PORTING_STAGE_REPORT.md",
        "15类倾向性审美.txt"
        src_doc = PROJECT_ROOT / doc

    # 2.1 Copy APK for GitHub Release page
    apk_file = PROJECT_ROOT / "Wreck-v2.2.apk"
    if apk_file.exists():
        print(f"  -> Copying {apk_file.name} to release directory...")
        shutil.copy2(apk_file, TARGET_DIR / apk_file.name)

    # 3. Create .gitignore
build/
bin/
obj/
out/
*.apk
*.idsig
*.class

# Raw binary dumps & test outputs
*.raw
tests/c_outputs/
tests/real_device_c_outputs/
tests/real_device_outputs/

# Local Portable Toolchains & SDKs (do not commit compiler & SDK binaries)
tools/android-sdk/
tools/jdk-17/
tools/kotlinc/
tools/platform-tools/
tools/zig-*/

# Native Binaries
*.so
*.dll
*.dylib
*.a
*.lib
*.exe
*.o

# Gradle & Android Studio Cache
.gradle/
local.properties
.idea/
*.iml
.cxx/

# Python Cache
__pycache__/
*.pyc

# OS & Temp
.DS_Store
Thumbs.db
*.tmp
*.log
*.keystore
"""
    with open(TARGET_DIR / ".gitignore", "w", encoding="utf-8") as f:
        f.write(gitignore_content)

    # 4. Create LICENSE (MIT)
    license_content = """MIT License

Copyright (c) 2026 Wreck Project Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""
    with open(TARGET_DIR / "LICENSE", "w", encoding="utf-8") as f:
        f.write(license_content)

    # 5. Create specialized GitHub README
    gh_readme = """# Wreck v2.2 - Real-Time Cinematic Adaptive Grading Engine (Android / C)

[![Platform](https://img.shields.io/badge/Platform-Android%2015-brightgreen.svg)]()
[![Arch](https://img.shields.io/badge/Architecture-ARM64--v8a%20%7C%20NEON-blue.svg)]()
[![Hardware](https://img.shields.io/badge/Target-Snapdragon%208%20Elite%20%7C%20OnePlus%2013-orange.svg)]()
[![License](https://img.shields.io/badge/License-MIT-green.svg)]()

**Wreck** 是面向旗舰移动端（专为高通骁龙 8 至尊版 / OnePlus 13 深度调优）打造的超低时延、全端侧实时的自适应电影级调色相机与图像处理引擎。


- **15 款殿堂级电影色彩美学风格 + 3 款超风格化滤镜**：
  - 港风复古、电影冷青、暗调情绪、复古暖阳、富士胶片、黑白银盐等 15 款经典电影风格。
  - **v2.2 新增三大超级风格化**：针对参考摄影样张深度学习拟真，推出 **Noon 正午**、**Golden Hour 黄金时刻** 与 **Sunset 落日余晖**，真实再现纯澈晴空蓝、鲜活翠绿草坪与暮色紫粉晚霞光感。
  - **全新左上角胶囊 UI**：在“分屏对比”右侧无缝挂载“超风格化”按钮与折叠交互栏。
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
  - 打包生成独立的 `Wreck-v2.2.apk` 仅约 **940 KB**；
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
├── tools/                  # 基准黄金测试向量验证脚本
└── resources/              # 超风格化光学参考样张 (Noon, Golden Hour, Sunset)
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
"""
    with open(TARGET_DIR / "README.md", "w", encoding="utf-8") as f:
        f.write(gh_readme)

    print(f"\n[SUCCESS] Clean GitHub release directory prepared at:\n  {TARGET_DIR}")

if __name__ == "__main__":
    main()

