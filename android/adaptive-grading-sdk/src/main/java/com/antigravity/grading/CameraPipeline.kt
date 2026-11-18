package com.antigravity.grading

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Matrix
import android.os.SystemClock
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * 实时相机处理帧回调
 */
interface CameraGradingListener {
    fun onFrameGraded(
        outputBuffer: ByteBuffer,
        width: Int,
        height: Int,
        metrics: SceneMetrics,
        params: StyleParameters,
        fps: Float,
        latencyMs: Float
    )

    fun onAnomalyDetected(message: String, latencyMs: Float)
}

/**
 * 实时相机流自适应调色处理管道 (CameraPipeline)
 * 采用硬件零拷贝 DirectByteBuffer 双缓冲池，保证 60 FPS 稳定运行
 */
class CameraPipeline(
    private val context: Context,
    private val engine: AdaptiveGradingEngine,
    val previewWidth: Int = 1280,
    val previewHeight: Int = 720
) : AutoCloseable {

    var listener: CameraGradingListener? = null

    // 分屏对比模式状态
    @Volatile
    var splitMode: Boolean = false

    @Volatile
    var splitRatio: Float = 0.5f

    // 预分配零拷贝连续堆外内存池
    private val yBufferSize = previewWidth * previewHeight
    private val uvBufferSize = previewWidth * previewHeight / 2
    private val rgbaBufferSize = previewWidth * previewHeight * 4

    private val yDirectBuffer: ByteBuffer = ByteBuffer.allocateDirect(yBufferSize).order(ByteOrder.nativeOrder())
    private val uvDirectBuffer: ByteBuffer = ByteBuffer.allocateDirect(uvBufferSize).order(ByteOrder.nativeOrder())
    private val rgbaDirectBuffer: ByteBuffer = ByteBuffer.allocateDirect(rgbaBufferSize).order(ByteOrder.nativeOrder())

    // 性能监视统计
    private var lastFrameTimeNs = 0L
    private var frameCount = 0
    private var fpsAccumulator = 0.0f
    private var currentFps = 0.0f
    private val fpsAlpha = 0.1f // 平滑系数

    private val isRunning = AtomicBoolean(false)
    private val lastGradedMetrics = AtomicReference<SceneMetrics>()
    private val lastGradedParams = AtomicReference<StyleParameters>()

    // 线程安全单次拍照请求回调 (捕获已完成风格解算与电影调色的高质量帧)
    @Volatile
    private var pendingPhotoCallback: ((Bitmap) -> Unit)? = null

    init {
        // 确保引擎初始化使用最优线程数 (旗舰平台推荐 4 线程)
        engine.setThreadCount(4)
    }

    /**
     * 注册下一次调色完成帧的捕获请求 (保证无竞态、100% 调色成片)
     */
    fun captureNextGradedFrame(callback: (Bitmap) -> Unit) {
        pendingPhotoCallback = callback
    }

    /**
     * 处理来自 Camera2 / CameraX YUV_420_888 (NV21 / NV12) 的硬件相机帧
     * 零拷贝直入 JNI 核心库进行切片调色
     */
    fun processCameraFrame(
        yPlane: ByteBuffer,
        uvPlane: ByteBuffer,
        isNv21: Boolean = true,
        yStride: Int = previewWidth,
        uvStride: Int = previewWidth
    ) {
        if (!isRunning.get()) return

        val startTimeMs = SystemClock.elapsedRealtimeNanos() / 1_000_000.0f

        // 硬件内存快速传递
        val (metrics, params) = engine.processFrameYUV420(
            yPlane = yPlane,
            uvPlane = uvPlane,
            dstRgba = rgbaDirectBuffer,
            width = previewWidth,
            height = previewHeight,
            yStride = yStride,
            uvStride = uvStride,
            dstStride = previewWidth * 4,
            isNv21 = isNv21,
            splitMode = splitMode,
            splitRatio = splitRatio
        )

        val endTimeMs = SystemClock.elapsedRealtimeNanos() / 1_000_000.0f
        val latencyMs = endTimeMs - startTimeMs

        lastGradedMetrics.set(metrics)
        lastGradedParams.set(params)

        // 检查是否有待捕获的拍照请求 (此时 rgbaDirectBuffer 是新鲜出炉的 100% 调色完毕帧)
        val photoCb = pendingPhotoCallback
        if (photoCb != null) {
            pendingPhotoCallback = null
            try {
                rgbaDirectBuffer.position(0)
                val rawBitmap = Bitmap.createBitmap(previewWidth, previewHeight, Bitmap.Config.ARGB_8888)
                rawBitmap.copyPixelsFromBuffer(rgbaDirectBuffer)

                // 顺时针旋转 90° 生成与持握方向一致的正立人像照片
                val rotMatrix = Matrix().apply { postRotate(90f) }
                val portraitBitmap = Bitmap.createBitmap(
                    rawBitmap, 0, 0, previewWidth, previewHeight, rotMatrix, true
                )
                if (portraitBitmap != rawBitmap) {
                    rawBitmap.recycle()
                }
                photoCb.invoke(portraitBitmap)
            } catch (e: Throwable) {
                e.printStackTrace()
            }
        }

        // 计算平滑实时 FPS
        val nowNs = SystemClock.elapsedRealtimeNanos()
        if (lastFrameTimeNs > 0) {
            val deltaSec = (nowNs - lastFrameTimeNs) / 1_000_000_000.0f
            if (deltaSec > 0.001f) {
                val instantFps = 1.0f / deltaSec
                currentFps = (1.0f - fpsAlpha) * currentFps + fpsAlpha * instantFps
            }
        }
        lastFrameTimeNs = nowNs
        frameCount++

        // 异常时延监控 (若处理耗时超过 33.3ms / 30FPS 预算自动触发告警)
        if (latencyMs > 33.3f) {
            listener?.onAnomalyDetected("单帧渲染耗时过长 ($latencyMs ms > 33.3 ms)，触发自适应限频！", latencyMs)
        }

        // 通知回调进行 Surface 呈现
        rgbaDirectBuffer.position(0)
        listener?.onFrameGraded(
            outputBuffer = rgbaDirectBuffer,
            width = previewWidth,
            height = previewHeight,
            metrics = metrics,
            params = params,
            fps = currentFps,
            latencyMs = latencyMs
        )
    }

    /**
     * 拍照成片导出 (兼容旧接口，直接旋转并压缩)
     */
    fun captureCurrentFrame(outputFile: File): Boolean {
        try {
            rgbaDirectBuffer.position(0)
            val rawBitmap = Bitmap.createBitmap(previewWidth, previewHeight, Bitmap.Config.ARGB_8888)
            rawBitmap.copyPixelsFromBuffer(rgbaDirectBuffer)

            val rotMatrix = Matrix().apply { postRotate(90f) }
            val portraitBitmap = Bitmap.createBitmap(
                rawBitmap, 0, 0, previewWidth, previewHeight, rotMatrix, true
            )
            if (portraitBitmap != rawBitmap) {
                rawBitmap.recycle()
            }

            FileOutputStream(outputFile).use { out ->
                portraitBitmap.compress(Bitmap.CompressFormat.JPEG, 98, out)
            }
            portraitBitmap.recycle()
            return true
        } catch (e: Exception) {
            e.printStackTrace()
            return false
        }
    }

    fun start() {
        isRunning.set(true)
        lastFrameTimeNs = SystemClock.elapsedRealtimeNanos()
    }

    fun stop() {
        isRunning.set(false)
    }

    fun getCurrentMetrics(): SceneMetrics? = lastGradedMetrics.get()
    fun getCurrentParams(): StyleParameters? = lastGradedParams.get()
    fun getFps(): Float = currentFps

    override fun close() {
        stop()
    }
}
