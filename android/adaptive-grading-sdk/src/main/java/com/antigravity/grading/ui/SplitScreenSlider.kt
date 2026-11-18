package com.antigravity.grading.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View

/**
 * 分屏对比实时拖拽滑块 (Split Screen Drag Slider)
 * 允许用户在取景器上左右自由拖动分屏线，实时对比调色前后差异
 */
class SplitScreenSlider @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    var splitRatio: Float = 0.5f
        set(value) {
            field = value.coerceIn(0.02f, 0.98f)
            postInvalidate()
            listener?.invoke(field)
        }

    var listener: ((Float) -> Unit)? = null

    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        strokeWidth = 4f
    }

    private val handleOuterPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }

    private val handleInnerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(40, 40, 45)
        style = Paint.Style.FILL
    }

    private var isDragging = false

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!isEnabled || visibility != VISIBLE) return false

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val touchX = event.x
                val currentX = width * splitRatio
                // 点击在滑块附近 80px 范围内或者直接点击响应
                if (Math.abs(touchX - currentX) < 120f || event.y in (height * 0.3f)..(height * 0.7f)) {
                    isDragging = true
                    parent?.requestDisallowInterceptTouchEvent(true)
                    splitRatio = (touchX / width.toFloat()).coerceIn(0.02f, 0.98f)
                    return true
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (isDragging) {
                    splitRatio = (event.x / width.toFloat()).coerceIn(0.02f, 0.98f)
                    return true
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (isDragging) {
                    isDragging = false
                    parent?.requestDisallowInterceptTouchEvent(false)
                    return true
                }
            }
        }
        return super.onTouchEvent(event)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        val x = w * splitRatio
        val centerY = h * 0.5f

        // 1. 垂直中线
        canvas.drawLine(x, 0f, x, h, linePaint)

        // 2. 居中拖拽圆钮
        val handleRadius = 36f
        canvas.drawCircle(x, centerY, handleRadius, handleOuterPaint)
        canvas.drawCircle(x, centerY, handleRadius - 6f, handleInnerPaint)

        // 3. 圆钮内左右箭头小点
        canvas.drawCircle(x - 12f, centerY, 4f, handleOuterPaint)
        canvas.drawCircle(x + 12f, centerY, 4f, handleOuterPaint)
    }
}
