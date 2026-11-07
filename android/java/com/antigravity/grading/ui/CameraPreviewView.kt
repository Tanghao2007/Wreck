package com.antigravity.grading.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.util.AttributeSet
import android.view.SurfaceHolder
import android.view.SurfaceView
import java.nio.ByteBuffer

/**
 * 实时相机调色画面高性能硬件渲染 SurfaceView
 * 自动应用 90° 旋转矫正与 Aspect-Fill (Center-Crop) 缩放，
 * 彻底消除画面拉伸形变与软模糊，直通 Adreno GPU 硬件加速渲染。
 */
class CameraPreviewView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : SurfaceView(context, attrs, defStyleAttr), SurfaceHolder.Callback {

    private var renderBitmap: Bitmap? = null
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)
    private val renderMatrix = Matrix()

    private var viewWidth = 0
    private var viewHeight = 0
    private var lastFrameWidth = 0
    private var lastFrameHeight = 0
    private var isSurfaceReady = false

    init {
        holder.addCallback(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        isSurfaceReady = true
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        viewWidth = width
        viewHeight = height
        updateTransformMatrix(lastFrameWidth, lastFrameHeight)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        isSurfaceReady = false
        renderBitmap?.recycle()
        renderBitmap = null
    }

    /**
     * 更新仿射变换矩阵：
     * 1. 旋转 90° 顺时针（抵消后置相机横向传感器在手机竖持时的 90° 逆时针物理偏移）
     * 2. 居中裁切等比例全屏填充（Aspect-Fill Center-Crop，无任何拉伸失真）
     */
    private fun updateTransformMatrix(frameW: Int, frameH: Int) {
        if (viewWidth <= 0 || viewHeight <= 0 || frameW <= 0 || frameH <= 0) return

        renderMatrix.reset()
        // 相机横屏帧顺时针旋转 90° 后的有效宽高
        val rotatedW = frameH.toFloat()
        val rotatedH = frameW.toFloat()

        // 等比填充屏幕最大缩放比 (Center-Crop)
        val scale = maxOf(viewWidth.toFloat() / rotatedW, viewHeight.toFloat() / rotatedH)

        // 步骤：原图中心移到原点 -> 顺时针旋转 90° -> 等比缩放 -> 移到屏幕中心
        renderMatrix.postTranslate(-frameW / 2f, -frameH / 2f)
        renderMatrix.postRotate(90f)
        renderMatrix.postScale(scale, scale)
        renderMatrix.postTranslate(viewWidth / 2f, viewHeight / 2f)

        lastFrameWidth = frameW
        lastFrameHeight = frameH
    }

    /**
     * 将 DirectByteBuffer 渲染帧绘制到硬件 Surface
     */
    fun renderFrame(buffer: ByteBuffer, frameWidth: Int, frameHeight: Int) {
        if (!isSurfaceReady) return

        // 尺寸变动时重新分配与重算矩阵
        if (renderBitmap == null || renderBitmap?.width != frameWidth || renderBitmap?.height != frameHeight) {
            renderBitmap?.recycle()
            renderBitmap = Bitmap.createBitmap(frameWidth, frameHeight, Bitmap.Config.ARGB_8888)
            updateTransformMatrix(frameWidth, frameHeight)
        } else if (frameWidth != lastFrameWidth || frameHeight != lastFrameHeight) {
            updateTransformMatrix(frameWidth, frameHeight)
        }

        val bitmap = renderBitmap ?: return
        buffer.position(0)
        bitmap.copyPixelsFromBuffer(buffer)

        var canvas: Canvas? = null
        try {
            canvas = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                holder.lockHardwareCanvas()
            } else {
                holder.lockCanvas()
            }
            if (canvas != null) {
                // 绘制 GPU 仿射矩阵变换帧
                canvas.drawBitmap(bitmap, renderMatrix, paint)
            }
        } catch (e: Exception) {
            e.printStackTrace()
        } finally {
            if (canvas != null) {
                try {
                    holder.unlockCanvasAndPost(canvas)
                } catch (e: Exception) {
                    e.printStackTrace()
                }
            }
        }
    }
}
