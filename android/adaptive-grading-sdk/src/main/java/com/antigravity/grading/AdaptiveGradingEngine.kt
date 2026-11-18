package com.antigravity.grading

import java.nio.ByteBuffer

/**
 * 低级 JNI 原生函数绑定声明
 */
object AdaptiveGradingNative {
    init {
        try {
            android.util.Log.i("GradingNative", "Loading adaptive_grading_jni...")
            System.loadLibrary("adaptive_grading_jni")
            android.util.Log.i("GradingNative", "Loaded adaptive_grading_jni successfully!")
        } catch (e: Throwable) {
            android.util.Log.e("GradingNative", "FATAL: System.loadLibrary(adaptive_grading_jni) threw: ${e::class.java.name}: ${e.message}", e)
        }
    }

    @JvmStatic
    external fun nativeCreateEngine(defaultStyleId: Int): Long

    @JvmStatic
    external fun nativeDestroyEngine(handle: Long)

    @JvmStatic
    external fun nativeSetStyle(handle: Long, styleId: Int): Int

    @JvmStatic
    external fun nativeGetStyle(handle: Long): Int

    @JvmStatic
    external fun nativeResetSmoother(handle: Long)

    @JvmStatic
    external fun nativeSetThreadCount(handle: Long, threadCount: Int): Int

    @JvmStatic
    external fun nativeGetThreadCount(handle: Long): Int

    @JvmStatic
    external fun nativeAnalyze(
        srcBuffer: ByteBuffer,
        width: Int,
        height: Int,
        stride: Int,
        format: Int,
        outMetrics: FloatArray
    ): Int

    @JvmStatic
    external fun nativeProcessFrameDirect(
        handle: Long,
        srcBuffer: ByteBuffer,
        dstBuffer: ByteBuffer,
        width: Int,
        height: Int,
        srcStride: Int,
        dstStride: Int,
        format: Int,
        splitMode: Boolean,
        splitRatio: Float,
        outMetrics: FloatArray?,
        outParams: FloatArray?
    ): Int

    @JvmStatic
    external fun nativeProcessFrameYUV420(
        handle: Long,
        yBuffer: ByteBuffer,
        uvBuffer: ByteBuffer,
        dstRgbaBuffer: ByteBuffer,
        width: Int,
        height: Int,
        yStride: Int,
        uvStride: Int,
        dstStride: Int,
        isNv21: Boolean,
        splitMode: Boolean,
        splitRatio: Float,
        outMetrics: FloatArray?,
        outParams: FloatArray?
    ): Int

    @JvmStatic
    external fun nativeDumpLogs(handle: Long, dumpPath: String): Int

    @JvmStatic
    external fun nativeGetVersion(): String

    @JvmStatic
    external fun nativeGradeBitmapInPlace(handle: Long, bitmap: android.graphics.Bitmap): Int
}

/**
 * 高级面向对象引擎封装 (管理 Native 句柄生命周期与零拷贝处理)
 */
class AdaptiveGradingEngine(defaultStyleId: Int = 1) : AutoCloseable {
    private var nativeHandle: Long = 0
    private val metricsBuffer = FloatArray(8)
    private val paramsBuffer = FloatArray(16)

    init {
        nativeHandle = AdaptiveGradingNative.nativeCreateEngine(defaultStyleId)
        if (nativeHandle == 0L) {
            throw IllegalStateException("无法创建 Native 调色引擎实例，可能内存不足！")
        }
    }

    fun setStyle(styleId: Int) {
        checkNotClosed()
        AdaptiveGradingNative.nativeSetStyle(nativeHandle, styleId)
    }

    fun getStyle(): Int {
        checkNotClosed()
        return AdaptiveGradingNative.nativeGetStyle(nativeHandle)
    }

    fun resetSmoother() {
        checkNotClosed()
        AdaptiveGradingNative.nativeResetSmoother(nativeHandle)
    }

    fun setThreadCount(count: Int) {
        checkNotClosed()
        AdaptiveGradingNative.nativeSetThreadCount(nativeHandle, count)
    }

    fun getThreadCount(): Int {
        checkNotClosed()
        return AdaptiveGradingNative.nativeGetThreadCount(nativeHandle)
    }

    /**
     * 极速单帧渲染处理 (DirectByteBuffer 零拷贝)
     */
    fun processFrame(
        src: ByteBuffer,
        dst: ByteBuffer,
        width: Int,
        height: Int,
        srcStride: Int,
        dstStride: Int,
        format: PixelFormat = PixelFormat.RGBA8888,
        splitMode: Boolean = false,
        splitRatio: Float = 0.5f
    ): Pair<SceneMetrics, StyleParameters> {
        checkNotClosed()
        require(src.isDirect && dst.isDirect) { "输入与输出缓冲区必须为 DirectByteBuffer 以实现硬件零拷贝！" }

        val res = AdaptiveGradingNative.nativeProcessFrameDirect(
            nativeHandle,
            src,
            dst,
            width,
            height,
            srcStride,
            dstStride,
            format.nativeId,
            splitMode,
            splitRatio,
            metricsBuffer,
            paramsBuffer
        )
        if (res != 0) {
            throw RuntimeException("Native 渲染流水线执行失败，错误码: $res")
        }

        return Pair(
            SceneMetrics.fromArray(metricsBuffer),
            StyleParameters.fromArray(paramsBuffer)
        )
    }

    /**
     * 相机原生流硬件直出 (Camera2 / CameraX YUV_420_888 / NV21 / NV12 零拷贝直入)
     */
    fun processFrameYUV420(
        yPlane: ByteBuffer,
        uvPlane: ByteBuffer,
        dstRgba: ByteBuffer,
        width: Int,
        height: Int,
        yStride: Int = width,
        uvStride: Int = width,
        dstStride: Int = width * 4,
        isNv21: Boolean = true,
        splitMode: Boolean = false,
        splitRatio: Float = 0.5f
    ): Pair<SceneMetrics, StyleParameters> {
        checkNotClosed()
        require(yPlane.isDirect && uvPlane.isDirect && dstRgba.isDirect) {
            "Y、UV 与输出缓冲区必须为 DirectByteBuffer 以实现硬件零拷贝！"
        }

        val res = AdaptiveGradingNative.nativeProcessFrameYUV420(
            nativeHandle,
            yPlane,
            uvPlane,
            dstRgba,
            width,
            height,
            yStride,
            uvStride,
            dstStride,
            isNv21,
            splitMode,
            splitRatio,
            metricsBuffer,
            paramsBuffer
        )
        if (res != 0) {
            throw RuntimeException("Native 相机 YUV420 流水线执行失败，错误码: $res")
        }

        return Pair(
            SceneMetrics.fromArray(metricsBuffer),
            StyleParameters.fromArray(paramsBuffer)
        )
    }

    /**
     * 高分辨率图像/照片原地硬件零拷贝调色 (直接操作 Bitmap 图形物理内存，无需任何额外分配)
     */
    fun gradeBitmap(bitmap: android.graphics.Bitmap): Boolean {
        checkNotClosed()
        val ret = AdaptiveGradingNative.nativeGradeBitmapInPlace(nativeHandle, bitmap)
        return ret == 0
    }

    fun dumpLogs(dumpPath: String) {
        checkNotClosed()
        AdaptiveGradingNative.nativeDumpLogs(nativeHandle, dumpPath)
    }

    override fun close() {
        if (nativeHandle != 0L) {
            AdaptiveGradingNative.nativeDestroyEngine(nativeHandle)
            nativeHandle = 0L
        }
    }

    private fun checkNotClosed() {
        check(nativeHandle != 0L) { "AdaptiveGradingEngine 已被释放，不可重复调用！" }
    }
}

