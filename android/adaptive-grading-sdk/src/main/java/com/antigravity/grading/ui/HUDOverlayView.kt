package com.antigravity.grading.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import android.view.animation.AccelerateDecelerateInterpolator
import com.antigravity.grading.STYLE_CATALOG
import com.antigravity.grading.SceneMetrics
import com.antigravity.grading.StyleInfo

/**
 * 现代高质感 OSD (On-Screen Display) 取景仪表盘覆盖层
 */
class HUDOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    var currentStyle: StyleInfo = STYLE_CATALOG[0]
        set(value) {
            field = value
            postInvalidate()
        }

    var metrics: SceneMetrics? = null
        set(value) {
            field = value
            postInvalidate()
        }

    var fps: Float = 0.0f
        set(value) {
            field = value
            postInvalidate()
        }

    var latencyMs: Float = 0.0f
        set(value) {
            field = value
            postInvalidate()
        }

    var isSplitMode: Boolean = false
        set(value) {
            field = value
            postInvalidate()
        }

    var isVisibleHUD: Boolean = true
        set(value) {
            field = value
            postInvalidate()
        }

    // 快门闪白动画
    private var flashAlpha: Float = 0.0f
    private val flashPaint = Paint().apply { color = Color.WHITE }

    // Toast 提示
    private var toastText: String? = null
    private var toastAlpha: Float = 0.0f

    // 绘制画笔与矩形
    private val bgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(180, 18, 18, 24) // 半透明黑深灰
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textSize = 36f
    }
    private val accentPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(80, 220, 255) // 青蓝高亮
        textSize = 38f
        isFakeBoldText = true
    }
    private val fpsGreenPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(120, 255, 120)
        textSize = 34f
        isFakeBoldText = true
    }
    private val fpsYellowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(255, 215, 0)
        textSize = 34f
        isFakeBoldText = true
    }
    private val fpsRedPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(255, 80, 80)
        textSize = 34f
        isFakeBoldText = true
    }
    private val capsuleRect = RectF()

    /**
     * 触发快门闪光动画
     */
    fun triggerShutterAnimation(message: String = "成片已保存至相册") {
        ValueAnimator.ofFloat(0.85f, 0.0f).apply {
            duration = 200
            interpolator = AccelerateDecelerateInterpolator()
            addUpdateListener {
                flashAlpha = it.animatedValue as Float
                postInvalidate()
            }
            start()
        }

        toastText = message
        ValueAnimator.ofFloat(0.0f, 1.0f, 1.0f, 0.0f).apply {
            duration = 2500
            addUpdateListener {
                toastAlpha = it.animatedValue as Float
                postInvalidate()
            }
            start()
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()

        // 1. 绘制快门闪白动画
        if (flashAlpha > 0.01f) {
            flashPaint.alpha = (flashAlpha * 255).toInt()
            canvas.drawRect(0f, 0f, w, h, flashPaint)
        }

        if (!isVisibleHUD) {
            drawToast(canvas, w, h)
            return
        }

        // 2. 顶部胶囊仪表盘 (Top Capsule OSD)
        val topPadding = 48f
        val capsuleHeight = 110f
        capsuleRect.set(24f, topPadding, w - 24f, topPadding + capsuleHeight)
        canvas.drawRoundRect(capsuleRect, 32f, 32f, bgPaint)

        // 风格名称与徽章
        val styleLabel = "[${currentStyle.id}/15] ${currentStyle.name}"
        accentPaint.color = currentStyle.colorHex.toInt()
        canvas.drawText(styleLabel, capsuleRect.left + 28f, capsuleRect.top + 52f, accentPaint)

        textPaint.textSize = 28f
        textPaint.color = Color.rgb(190, 195, 205)
        canvas.drawText(currentStyle.englishName, capsuleRect.left + 28f, capsuleRect.top + 92f, textPaint)

        // 居中场景情报 (色温/场景分类/动态范围)
        val m = metrics
        if (m != null) {
            val sceneStr = "${m.sceneType.label.split(' ')[0]} | ${m.estimatedCct.toInt()}K | DR: ${String.format("%.1f", m.dynamicRangeEv)} EV"
            textPaint.textSize = 30f
            textPaint.color = Color.rgb(230, 230, 235)
            val sceneWidth = textPaint.measureText(sceneStr)
            val centerLeft = (w - sceneWidth) / 2f
            canvas.drawText(sceneStr, centerLeft, capsuleRect.top + 68f, textPaint)
        }

        // 右侧实时 FPS 与渲染耗时
        val fpsStr = String.format("FPS: %4.1f", fps)
        val latencyStr = String.format("%4.1f ms", latencyMs)
        val fpsP = when {
            fps >= 30f -> fpsGreenPaint
            fps >= 20f -> fpsYellowPaint
            else -> fpsRedPaint
        }
        val fpsWidth = fpsP.measureText(fpsStr)
        val rightX = capsuleRect.right - fpsWidth - 28f
        canvas.drawText(fpsStr, rightX, capsuleRect.top + 52f, fpsP)

        textPaint.textSize = 26f
        textPaint.color = Color.rgb(160, 165, 175)
        canvas.drawText(latencyStr, rightX + (fpsWidth - textPaint.measureText(latencyStr)) / 2f, capsuleRect.top + 92f, textPaint)

        // 3. 分屏对比标签指示器
        if (isSplitMode) {
            textPaint.textSize = 34f
            textPaint.color = Color.rgb(200, 200, 200)
            canvas.drawText("ORIGINAL", 36f, capsuleRect.bottom + 60f, textPaint)

            accentPaint.textSize = 34f
            val gradedStr = "GRADED"
            val gradedW = accentPaint.measureText(gradedStr)
            canvas.drawText(gradedStr, w - gradedW - 36f, capsuleRect.bottom + 60f, accentPaint)
        }

        // 4. Toast 提示
        drawToast(canvas, w, h)
    }

    private fun drawToast(canvas: Canvas, w: Float, h: Float) {
        val msg = toastText ?: return
        if (toastAlpha <= 0.01f) return

        textPaint.textSize = 32f
        val textW = textPaint.measureText(msg)
        val toastRect = RectF(
            (w - textW) / 2f - 40f,
            h * 0.72f,
            (w + textW) / 2f + 40f,
            h * 0.72f + 80f
        )

        bgPaint.alpha = (toastAlpha * 220).toInt()
        canvas.drawRoundRect(toastRect, 40f, 40f, bgPaint)

        textPaint.color = Color.WHITE
        textPaint.alpha = (toastAlpha * 255).toInt()
        canvas.drawText(msg, (w - textW) / 2f, toastRect.centerY() + 11f, textPaint)
    }
}
