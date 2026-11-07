# Adaptive Grading Android SDK

**自适应实时电影级调色引擎 Android 原生开发套件 (v2.0.0)**

---

## 🌟 核心特性
- **纯端侧极速运算**：无任何云端网络依赖，基于 ARM64 NEON 指令集与多核微秒级无锁自旋线程池优化。
- **15 款电影风格实时渲染**：自然人像、日系清透、黑金夜景、赛博朋克、复古胶片、电影青橙等。
- **45~50 FPS 720p 实时预览**：在骁龙 8 至尊版（Snapdragon 8 Elite）单帧渲染仅需 19.8ms ~ 22.7ms。
- **零拷贝流水线**：DirectByteBuffer 硬件级直通，Camera2 / CameraX YUV420_888 (NV21/NV12) 直接调色着色至 RGBA。
- **交互级分屏对比**：支持任意比例实时原图/调色分屏推拉与分割线高光渲染。
- **极小二进制体积**：动态链接库仅 165 KB，零第三方 C/C++ 动态依赖。

---

## 📦 依赖引入

### Gradle (Kotlin DSL)
```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        mavenCentral()
        mavenLocal()
    }
}

// build.gradle.kts (:app)
dependencies {
    implementation("com.antigravity:adaptive-grading-sdk:2.0.0")
}
```

---

## 🚀 快速接入

### 1. 引擎初始化与释放
```kotlin
import com.antigravity.grading.AdaptiveGradingEngine
import com.antigravity.grading.PixelFormat

// 在 Activity 或 ViewModel 中创建实例 (传入初始风格 ID，例如 1: 自然人像)
val gradingEngine = AdaptiveGradingEngine(initialStyleId = 1)

// 动态切换风格 (1 ~ 15)
gradingEngine.setStyle(4) // 4: 胶片复古

// 设置多核线程池工作核心数 (推荐 4 线程，均衡性能与功耗)
gradingEngine.setThreadCount(4)

// 页面销毁时释放底层 Native 上下文
override fun onDestroy() {
    super.onDestroy()
    gradingEngine.release()
}
```

### 2. 相机帧实时调色 (CameraX ImageAnalysis 零拷贝接入)
```kotlin
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import java.nio.ByteBuffer

val imageAnalyzer = ImageAnalysis.Builder()
    .setTargetResolution(android.util.Size(1280, 720))
    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
    .build()

// 预分配复用的 DirectByteBuffer (1280 * 720 * 4 字节)
val outRgbaBuffer: ByteBuffer = ByteBuffer.allocateDirect(1280 * 720 * 4)

imageAnalyzer.setAnalyzer(cameraExecutor) { imageProxy ->
    try {
        val yBuffer = imageProxy.planes[0].buffer
        val uvBuffer = imageProxy.planes[1].buffer
        val isNv21 = (imageProxy.planes[2].buffer == imageProxy.planes[1].buffer)

        // Native 硬件流调色运算 (单帧 ~20ms)
        gradingEngine.processYuv420(
            yPlane = yBuffer,
            uvPlane = uvBuffer,
            dstRgba = outRgbaBuffer,
            width = imageProxy.width,
            height = imageProxy.height,
            isNv21 = isNv21,
            splitMode = false,
            splitRatio = 0.5f
        )

        // outRgbaBuffer 现已包含渲染完成的高品质电影色调图像
        // 可直接交由 SurfaceView 或 OpenGL 纹理刷屏显示
    } finally {
        imageProxy.close()
    }
}
```

### 3. 分屏推拉对比模式
```kotlin
// 开启分屏模式，左侧原图，右侧调色，比例 0.5 (居中)
gradingEngine.processYuv420(
    yPlane = yBuffer,
    uvPlane = uvBuffer,
    dstRgba = outRgbaBuffer,
    width = 1280,
    height = 720,
    isNv21 = true,
    splitMode = true,
    splitRatio = 0.5f // 随着用户手指拖拽手势动态调整 0.0f ~ 1.0f
)
```

### 4. 配方持久化导出与加载 (SQLite / Room / File)
```kotlin
import com.antigravity.grading.AdaptiveStorage

// 导出当前调色参数配方为紧凑型二进制文件
AdaptiveStorage.exportRecipeBinary(
    engine = gradingEngine,
    filePath = "${context.filesDir}/my_custom_film_recipe.bin",
    recipeName = "Vintage Golden Hour",
    author = "Cinematographer"
)

// 加载外部配方并即时生效
AdaptiveStorage.importRecipeBinary(
    engine = gradingEngine,
    filePath = "${context.filesDir}/my_custom_film_recipe.bin"
)
```

---

## 📊 15 款电影风格清单

| ID | 风格名称 (Style Name) | 核心色调倾向 | 推荐拍摄场景 |
|:---|:---|:---|:---|
| 01 | **自然人像 (Natural Portrait)** | 暖肤透亮、柔和高光 | 室内人像、自然光日常 |
| 02 | **富士胶片 (Classic Chrome)** | 经典微红反转片、微硬对比 | 城市纪实、午后光影 |
| 03 | **日系清透 (Japanese Air)** | 高调低反差、蓝青阴影 | 校园、静物、阳光午后 |
| 04 | **胶片复古 (Vintage Film)** | 胶片褪色质感、暖黄暗角 | 怀旧咖啡厅、街头古建 |
| 05 | **莫兰迪 (Morandi Neutral)** | 柔和低饱和度、灰调平衡 | 极简建筑、艺术展陈 |
| 06 | **落日余晖 (Sunset Glow)** | 金橙高光、紫粉阴影 | 黄金时刻、海边晚霞 |
| 07 | **森林晨曦 (Forest Breeze)** | 翠绿层次分明、冷白点光 | 森林公园、露营徒步 |
| 08 | **电影青橙 (Teal & Orange)** | 极致冷暖冲撞、经典好莱坞 | 商业大片、夜市街拍 |
| 09 | **北欧极简 (Nordic Chill)** | 极寒冷蓝基底、高清晰度 | 雪景、极简工业设计 |
| 10 | **黑金夜景 (Black & Gold)** | 压暗冷灰杂色、高亮金色霓虹 | 繁华CBD夜景、夜行车流 |
| 11 | **赛博朋克 (Cyberpunk 2077)** | 霓虹紫红高光、冷电青阴影 | 霓虹灯街区、雨夜倒影 |
| 12 | **王家卫风格 (Hong Kong Mood)** | 昏黄绿调、微抖动年代感 | 老香港街肆、复古室内 |
| 13 | **暮光之城 (Twilight Blue)** | 苍白冷青色调、幽静冷峻 | 阴天海岸、雨林雾气 |
| 14 | **阳光海滩 (Miami Sunshine)** | 高饱和明艳、蔚蓝碧海 | 热带海滨、假日泳池 |
| 15 | **黑白胶片 (Monochrome Noir)** | 细腻灰阶过渡、深沉银盐高反差 | 经典肖像、黑白人文光影 |

---

## 🛡️ 架构与系统兼容性
- **NDK ABI**: `arm64-v8a` (纯 64 位支持，符合 Google Play 规范)
- **Min Android SDK**: API 24 (Android 7.0+)
- **Target Android SDK**: API 35 (Android 15)
- **内存开销**: 运行期常驻物理内存稳定在 **18MB ~ 28MB**，单帧运算零内存抖动。
- **隐私合规**: 零网络通信，不申请 `INTERNET` 权限，完全符合 GDPR 与 PIPL 合规准则。

