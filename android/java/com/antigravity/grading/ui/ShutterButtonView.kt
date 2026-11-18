package com.antigravity.grading.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.util.AttributeSet
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.animation.LinearInterpolator

/**
 * 专业级相机快门与长按录像多功能按键
 * 1. 轻触 (< 2秒)：触发拍照 (takePhoto)
 * 2. 长按约 2 秒：环形进度条蓄力，蓄力完成震动并开启视频与传感器录制；松手后继续保持录像
 * 3. 录制状态下再次轻触：立即结束录制并保存成片
 */
class ShutterButtonView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    enum class State {
        IDLE,
        PRESSING,
        RECORDING
    }

    var currentState = State.IDLE
        private set

    var onPhotoClick: (() -> Unit)? = null
    var onRecordStart: (() -> Unit)? = null
    var onRecordStop: (() -> Unit)? = null

    private var pressProgress: Float = 0.0f
    private var progressAnimator: ValueAnimator? = null
    private val LONG_PRESS_DURATION_MS = 1800L // 约 2 秒蓄力门槛 (1.8s 饱满环形动画)

    // 标记当前触摸是否已触发长按，避免长按松手时被误识别为再次轻触停止
    private var hasTriggeredLongPress = false
    private var isAnimationCancelled = false

    private val outerRingPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 8f
        color = Color.WHITE
    }

    private val progressPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 10f
        strokeCap = Paint.Cap.ROUND
        color = Color.rgb(255, 59, 48) // 红色蓄力进度
    }

    private val innerCirclePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.WHITE
    }

    private val rectF = RectF()
    private val innerRectF = RectF()

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val size = 180
        setMeasuredDimension(size, size)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!isEnabled) return false

        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                if (currentState == State.RECORDING) {
                    // 录制进行中按下，等待松手以触发停止录制
                    return true
                }
                hasTriggeredLongPress = false
                isAnimationCancelled = false
                currentState = State.PRESSING
                startProgressAnimation()
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (hasTriggeredLongPress) {
                    // 用户刚完成 2 秒长按录制触发后的初次抬手，保持录像继续运行
                    hasTriggeredLongPress = false
                    return true
                }

                if (currentState == State.RECORDING) {
                    // 再次轻触：立即结束录制
                    stopRecordingState()
                    onRecordStop?.invoke()
                    performHaptic()
                    return true
                }

                if (currentState == State.PRESSING) {
                    cancelProgressAnimation()
                    currentState = State.IDLE
                    // 未满 2 秒抬手：执行普通拍照
                    onPhotoClick?.invoke()
                    performHaptic()
                    invalidate()
                    return true
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                if (currentState == State.PRESSING) {
                    cancelProgressAnimation()
                    currentState = State.IDLE
                    hasTriggeredLongPress = false
                    invalidate()
                }
            }
        }
        return super.onTouchEvent(event)
    }

    private fun startProgressAnimation() {
        cancelProgressAnimation()
        isAnimationCancelled = false
        progressAnimator = ValueAnimator.ofFloat(0.0f, 1.0f).apply {
            duration = LONG_PRESS_DURATION_MS
            interpolator = LinearInterpolator()
            addUpdateListener {
                pressProgress = it.animatedValue as Float
                invalidate()
            }
            addListener(object : android.animation.AnimatorListenerAdapter() {
                override fun onAnimationCancel(animation: android.animation.Animator) {
                    isAnimationCancelled = true
                }

                override fun onAnimationEnd(animation: android.animation.Animator) {
                    if (!isAnimationCancelled && currentState == State.PRESSING) {
                        // 蓄力满 2 秒，触发长按录像
                        hasTriggeredLongPress = true
                        currentState = State.RECORDING
                        pressProgress = 0.0f
                        performHaptic(isLongPress = true)
                        onRecordStart?.invoke()
                        invalidate()
                    }
                }
            })
            start()
        }
    }

    private fun cancelProgressAnimation() {
        isAnimationCancelled = true
        progressAnimator?.let { anim ->
            anim.removeAllListeners()
            anim.cancel()
        }
        progressAnimator = null
        pressProgress = 0.0f
    }

    fun stopRecordingState() {
        cancelProgressAnimation()
        hasTriggeredLongPress = false
        currentState = State.IDLE
        pressProgress = 0.0f
        invalidate()
    }

    private fun performHaptic(isLongPress: Boolean = false) {
        try {
            if (isLongPress) {
                val vibrator = context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    vibrator?.vibrate(VibrationEffect.createOneShot(70, VibrationEffect.DEFAULT_AMPLITUDE))
                } else {
                    @Suppress("DEPRECATION")
                    vibrator?.vibrate(70)
                }
            } else {
                performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
            }
        } catch (_: Throwable) {}
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        val cx = w / 2f
        val cy = h / 2f
        val radius = (minOf(w, h) / 2f) - 10f

        when (currentState) {
            State.IDLE -> {
                // 1. 白色外圈
                outerRingPaint.color = Color.WHITE
                outerRingPaint.strokeWidth = 8f
                canvas.drawCircle(cx, cy, radius, outerRingPaint)

                // 2. 白色实心内圆
                innerCirclePaint.color = Color.WHITE
                canvas.drawCircle(cx, cy, radius - 16f, innerCirclePaint)
            }
            State.PRESSING -> {
                // 1. 半透明白色底圈
                outerRingPaint.color = Color.argb(120, 255, 255, 255)
                outerRingPaint.strokeWidth = 8f
                canvas.drawCircle(cx, cy, radius, outerRingPaint)

                // 2. 蓄力进度圆弧 (0 ~ 360 度)
                rectF.set(cx - radius, cy - radius, cx + radius, cy + radius)
                canvas.drawArc(rectF, -90f, pressProgress * 360f, false, progressPaint)

                // 3. 内圆略微缩放
                innerCirclePaint.color = Color.WHITE
                val innerR = (radius - 16f) * (1.0f - pressProgress * 0.15f)
                canvas.drawCircle(cx, cy, innerR, innerCirclePaint)
            }
            State.RECORDING -> {
                // 1. 录制状态：亮红外圈
                outerRingPaint.color = Color.rgb(255, 59, 48)
                outerRingPaint.strokeWidth = 8f
                canvas.drawCircle(cx, cy, radius, outerRingPaint)

                // 2. 录制状态：内部红色圆角矩形 (经典停止录像标识)
                innerCirclePaint.color = Color.rgb(255, 59, 48)
                val squareSize = (radius - 20f) * 0.75f
                innerRectF.set(cx - squareSize, cy - squareSize, cx + squareSize, cy + squareSize)
                canvas.drawRoundRect(innerRectF, 16f, 16f, innerCirclePaint)
            }
        }
    }
}
