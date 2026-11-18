package com.antigravity.grading.ui

import android.Manifest
import android.app.Activity
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.ImageFormat
import android.graphics.Matrix
import android.graphics.drawable.GradientDrawable
import android.hardware.camera2.*
import android.media.ImageReader
import android.media.MediaScannerConnection
import android.os.*
import android.provider.MediaStore
import android.view.Gravity
import android.view.ScaleGestureDetector
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import com.antigravity.grading.AdaptiveGradingEngine
import com.antigravity.grading.CameraGradingListener
import com.antigravity.grading.CameraPipeline
import com.antigravity.grading.STYLE_CATALOG
import com.antigravity.grading.SUPER_STYLE_CATALOG
import com.antigravity.grading.getStyleInfoById
import com.antigravity.grading.SceneMetrics
import com.antigravity.grading.StyleParameters
import com.antigravity.grading.recorder.SensorDataLogger
import com.antigravity.grading.recorder.VideoRecorder
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 实时自适应电影调色相机主界面 Activity
 * 实现了完整的相机生命周期、长短焦平滑变焦、长按 2s 录像、多风格无缝切换、
 * 陀螺仪原始数据采样、每帧时间戳记录与相机内参标准化导出。
 */
class CameraActivity : Activity(), CameraGradingListener {

    private lateinit var engine: AdaptiveGradingEngine
    private lateinit var pipeline: CameraPipeline
    private lateinit var videoRecorder: VideoRecorder
    private lateinit var sensorDataLogger: SensorDataLogger

    private lateinit var previewView: CameraPreviewView
    private lateinit var hudView: HUDOverlayView
    private lateinit var splitSlider: SplitScreenSlider
    private lateinit var styleSelector: StyleSelectorView
    private lateinit var shutterBtn: ShutterButtonView

    // 超风格化组件与状态 (Super Stylization: 正午 / 黄金时刻 / 落日余晖)
    private var superStyleBar: LinearLayout? = null
    private var superStyleBtn: View? = null
    private var activeSuperStyleId: Int? = null
    private val superStyleOptionViews = mutableMapOf<Int, Pair<LinearLayout, TextView>>()

    // 变焦组件与状态
    private var currentZoomRatio: Float = 1.0f
    private var minZoomRatio: Float = 0.6f
    private var maxZoomRatio: Float = 20.0f
    private val zoomButtonViews = mutableMapOf<Float, TextView>()
    private lateinit var scaleGestureDetector: ScaleGestureDetector

    // 录像状态与计时器
    private val isRecordingActive = AtomicBoolean(false)
    private var recordingDurationSec: Long = 0L
    private val timerHandler = Handler(Looper.getMainLooper())
    private var timerRunnable: Runnable? = null
    private var currentSessionPrefix: String = ""
    private var currentRecordingDir: File? = null
    private var currentVideoFile: File? = null

    // Camera2 硬件流组件
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var previewRequestBuilder: CaptureRequest.Builder? = null
    private var imageReader: ImageReader? = null
    private var stillImageReader: ImageReader? = null
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private val isCameraFeedActive = AtomicBoolean(false)

    // 每帧 CaptureCallback 监控与时间戳同步
    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(
            session: CameraCaptureSession,
            request: CaptureRequest,
            result: TotalCaptureResult
        ) {
            sensorDataLogger.currentCaptureResult = result
        }
    }

    // 备用实时渲染发生器 (相机未就绪或未授权时保证界面 60FPS 实时可交互)
    private var simulationThread: Thread? = null
    private val isSimulationRunning = AtomicBoolean(false)

    private val PERMISSION_REQ_CODE = 1001

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 1. 初始化 C 调色引擎、录像器与高频传感器记录器
        try {
            engine = AdaptiveGradingEngine(defaultStyleId = 1)
            pipeline = CameraPipeline(this, engine, previewWidth = 1280, previewHeight = 720)
            pipeline.listener = this
            videoRecorder = VideoRecorder(this, videoWidth = 720, videoHeight = 1280, frameWidth = 1280, frameHeight = 720)
            sensorDataLogger = SensorDataLogger(this)
        } catch (e: Throwable) {
            android.util.Log.e("CameraActivity", "初始化异常", e)
            Toast.makeText(this, "组件初始化失败: ${e.message}", Toast.LENGTH_LONG).show()
            finish()
            return
        }

        // 2. 动态组装高质感现代全屏交互视图结构
        val rootLayout = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
        }

        // 底层: 实时调色渲染 Surface
        previewView = CameraPreviewView(this)
        rootLayout.addView(previewView, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)

        // 绑定双指缩放手势识别器
        scaleGestureDetector = ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                val scale = detector.scaleFactor
                applyZoom(currentZoomRatio * scale)
                return true
            }
        })
        previewView.setOnTouchListener { _, event ->
            scaleGestureDetector.onTouchEvent(event)
            true
        }

        // 中层: 分屏对比拖拽中线 (默认隐藏，开启分屏模式时显示)
        splitSlider = SplitScreenSlider(this).apply {
            visibility = View.GONE
            listener = { ratio ->
                pipeline.splitRatio = ratio
            }
        }
        rootLayout.addView(splitSlider, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)

        // 顶层: OSD 状态与信息胶囊覆盖层 (含录像计时、实时变焦倍率)
        hudView = HUDOverlayView(this).apply {
            currentStyle = STYLE_CATALOG[0]
            zoomRatio = currentZoomRatio
        }
        rootLayout.addView(hudView, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)

        // 浮层控制栏: 顶部操作按钮 (分屏对比切换、超风格化、HUD 显示切换、日志导出)
        val topBar = createTopBar()
        rootLayout.addView(
            topBar,
            FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.TOP
                topMargin = 170
            }
        )

        // 超风格化悬浮菜单栏 (默认 GONE，点击超风格化按钮后展开)
        val superBar = createSuperStyleBar()
        superStyleBar = superBar
        rootLayout.addView(
            superBar,
            FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.TOP or Gravity.START
                topMargin = 260
                leftMargin = 40
            }
        )

        // 底部控制底栏: 快捷变焦胶囊 + 15款风格轮播条 + 快门/长按录像按键
        val bottomBar = createBottomBar()
        rootLayout.addView(
            bottomBar,
            FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.BOTTOM
                bottomMargin = 60
            }
        )

        setContentView(rootLayout)

        rootLayout.post {
            setImmersiveFullScreen()
        }

        // 3. 启动后台模拟流保证即刻有动态画面
        startSimulationFeed()

        // 4. 检查并申请权限
        checkAppPermissions()
    }

    private fun createTopBar(): View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(40, 0, 40, 0)

            // 1. 分屏对比切换按钮
            val splitBtn = createPillButton("分屏对比") {
                pipeline.splitMode = !pipeline.splitMode
                hudView.isSplitMode = pipeline.splitMode
                splitSlider.visibility = if (pipeline.splitMode) View.VISIBLE else View.GONE
                (it as? LinearLayout)?.let { pill ->
                    val bg = pill.background as? GradientDrawable
                    bg?.setColor(if (pipeline.splitMode) Color.rgb(80, 220, 255) else Color.argb(160, 25, 25, 30))
                }
            }
            addView(splitBtn)

            // 2. 超风格化按钮 (Super Stylization) - 位于分屏对比按钮右侧水平处
            val superBtn = createPillButton("超风格化") {
                toggleSuperStyleBar()
            }
            superStyleBtn = superBtn
            val superLp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                leftMargin = 16
            }
            addView(superBtn, superLp)

            // 弹性间隔
            addView(View(context), LinearLayout.LayoutParams(0, 1, 1f))

            // 3. HUD 仪表盘开关按钮
            val hudBtn = createPillButton("仪表盘") {
                hudView.isVisibleHUD = !hudView.isVisibleHUD
            }
            addView(hudBtn)

            // 4. 日志导出按钮
            val dumpBtn = createPillButton("导出日志") {
                dumpPerformanceLogs()
            }
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                leftMargin = 18
            }
            addView(dumpBtn, lp)
        }
    }

    private fun createBottomBar(): View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL

            // 1. 快捷变焦胶囊栏 (.6x 超广短焦 / 1x 主摄 / 3x 潜望长焦)
            val zoomBar = createZoomBar()
            addView(
                zoomBar,
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    gravity = Gravity.CENTER_HORIZONTAL
                    bottomMargin = 18
                }
            )

            // 2. 15 款电影风格轮播器 (录像前、录像中均可随时无缝切换风格)
            styleSelector = StyleSelectorView(context).apply {
                onStyleSelected = { style ->
                    activeSuperStyleId = null
                    updateSuperStyleViews()
                    engine.setStyle(style.id)
                    hudView.currentStyle = style
                }
            }
            addView(
                styleSelector,
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            )

            // 3. 拍摄快门与长按录像控制按键
            val shutterArea = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER
                setPadding(0, 24, 0, 10)

                shutterBtn = ShutterButtonView(context).apply {
                    onPhotoClick = {
                        takePhoto()
                    }
                    onRecordStart = {
                        startVideoRecording()
                    }
                    onRecordStop = {
                        stopVideoRecording()
                    }
                }
                addView(shutterBtn)
            }
            addView(shutterArea)
        }
    }

    /**
     * 快捷变焦栏：提供 0.6x (短焦)、1.0x (主摄)、3.0x (长焦) 原生一键直达
     */
    private fun createZoomBar(): View {
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(16, 8, 16, 8)
            background = GradientDrawable().apply {
                cornerRadius = 32f
                setColor(Color.argb(150, 18, 18, 24))
            }
        }

        val presets = listOf(0.6f, 1.0f, 3.0f)
        for (ratio in presets) {
            val label = when (ratio) {
                0.6f -> ".6"
                1.0f -> "1x"
                3.0f -> "3x"
                else -> "${ratio.toInt()}x"
            }
            val tv = TextView(this).apply {
                text = label
                textSize = 13f
                setPadding(26, 10, 26, 10)
                gravity = Gravity.CENTER
                isClickable = true
                isFocusable = true
                setOnClickListener {
                    applyZoom(ratio)
                }
            }
            zoomButtonViews[ratio] = tv
            bar.addView(tv)
        }
        updateZoomButtonsHighlight()
        return bar
    }

    private fun updateZoomButtonsHighlight() {
        for ((ratio, tv) in zoomButtonViews) {
            val isSelected = Math.abs(currentZoomRatio - ratio) < 0.25f
            val shape = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                if (isSelected) {
                    setColor(Color.rgb(80, 220, 255))
                } else {
                    setColor(Color.TRANSPARENT)
                }
            }
            tv.background = shape
            tv.setTextColor(if (isSelected) Color.BLACK else Color.WHITE)
            tv.paint.isFakeBoldText = isSelected
        }
    }

    private fun applyZoom(targetRatio: Float) {
        currentZoomRatio = targetRatio.coerceIn(minZoomRatio, maxZoomRatio)
        hudView.zoomRatio = currentZoomRatio
        updateZoomButtonsHighlight()

        val builder = previewRequestBuilder ?: return
        val session = captureSession ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, currentZoomRatio)
            try {
                session.setRepeatingRequest(builder.build(), captureCallback, cameraHandler)
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
    }

    private fun createPillButton(text: String, onClick: (View) -> Unit): View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            setPadding(28, 14, 28, 14)
            val shape = GradientDrawable().apply {
                cornerRadius = 24f
                setColor(Color.argb(160, 25, 25, 30))
                setStroke(1, Color.argb(80, 255, 255, 255))
            }
            background = shape
            isClickable = true
            isFocusable = true
            setOnClickListener(onClick)

            addView(android.widget.TextView(context).apply {
                this.text = text
                this.textSize = 12f
                this.setTextColor(Color.WHITE)
            })
        }
    }

    /**
     * 构建超风格化 (Super Stylization) 展开栏
     * 包含仿照 resource/ 三张照片的超风格：Noon 正午、Golden Hour 黄金时刻、Sunset 落日余晖
     */
    private fun createSuperStyleBar(): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            visibility = View.GONE
            setPadding(16, 12, 16, 12)
            val shape = GradientDrawable().apply {
                cornerRadius = 24f
                setColor(Color.argb(220, 18, 18, 24))
                setStroke(2, Color.argb(140, 255, 180, 50))
            }
            background = shape

            // 标题微标签
            val tagView = TextView(context).apply {
                text = "超风格"
                textSize = 11f
                setTextColor(Color.rgb(255, 200, 100))
                paint.isFakeBoldText = true
                setPadding(12, 0, 14, 0)
            }
            addView(tagView)

            // 遍历 SUPER_STYLE_CATALOG (16: 正午, 17: 黄金时刻, 18: 落日余晖)
            SUPER_STYLE_CATALOG.forEachIndexed { index, style ->
                val pill = LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER
                    setPadding(22, 10, 22, 10)
                    isClickable = true
                    isFocusable = true

                    val pillBg = GradientDrawable().apply {
                        cornerRadius = 18f
                        setColor(Color.argb(120, 35, 35, 42))
                        setStroke(1, Color.argb(60, 255, 255, 255))
                    }
                    background = pillBg

                    val label = TextView(context).apply {
                        text = when (style.id) {
                            16 -> "Noon 正午"
                            17 -> "Golden Hour 黄金时刻"
                            18 -> "Sunset 落日余晖"
                            else -> style.name
                        }
                        textSize = 12f
                        setTextColor(Color.rgb(220, 220, 230))
                    }
                    addView(label)

                    setOnClickListener {
                        selectSuperStyle(style.id)
                    }
                }

                val lp = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    if (index > 0) leftMargin = 10
                }
                addView(pill, lp)
                superStyleOptionViews[style.id] = Pair(pill, pill.getChildAt(0) as TextView)
            }

            // 关闭/折叠按钮
            val closeBtn = TextView(context).apply {
                text = "✕"
                textSize = 13f
                setTextColor(Color.rgb(180, 180, 190))
                setPadding(14, 6, 8, 6)
                isClickable = true
                setOnClickListener {
                    visibility = View.GONE
                }
            }
            addView(closeBtn)
        }
    }

    private fun selectSuperStyle(styleId: Int) {
        val style = getStyleInfoById(styleId)
        activeSuperStyleId = styleId
        engine.setStyle(styleId)
        hudView.currentStyle = style

        // 清除底部 15 款选择高亮
        styleSelector.clearSelection()

        // 更新顶部与浮层按钮高亮
        updateSuperStyleViews()

        // 闪白反馈并显示 Toast
        hudView.triggerShutterAnimation("已切换至 ${style.name}")
    }

    private fun updateSuperStyleViews() {
        val isActive = activeSuperStyleId != null && activeSuperStyleId!! in 16..18

        // 顶部“超风格化”胶囊按钮高亮
        superStyleBtn?.let { btn ->
            val bg = btn.background as? GradientDrawable
            if (isActive) {
                val color = when (activeSuperStyleId) {
                    16 -> Color.rgb(64, 196, 255)
                    17 -> Color.rgb(255, 179, 0)
                    18 -> Color.rgb(255, 87, 34)
                    else -> Color.rgb(255, 180, 50)
                }
                bg?.setColor(Color.argb(220, 40, 45, 55))
                bg?.setStroke(2, color)
                (btn as? LinearLayout)?.let { layout ->
                    (layout.getChildAt(0) as? TextView)?.setTextColor(color)
                    (layout.getChildAt(0) as? TextView)?.paint?.isFakeBoldText = true
                }
            } else {
                bg?.setColor(Color.argb(160, 25, 25, 30))
                bg?.setStroke(1, Color.argb(80, 255, 255, 255))
                (btn as? LinearLayout)?.let { layout ->
                    (layout.getChildAt(0) as? TextView)?.setTextColor(Color.WHITE)
                    (layout.getChildAt(0) as? TextView)?.paint?.isFakeBoldText = false
                }
            }
        }

        // 超级风格三个选项的高亮状态
        superStyleOptionViews.forEach { (id, pair) ->
            val (layout, textView) = pair
            val bg = layout.background as? GradientDrawable
            if (id == activeSuperStyleId) {
                val accentColor = when (id) {
                    16 -> Color.rgb(64, 196, 255)
                    17 -> Color.rgb(255, 179, 0)
                    18 -> Color.rgb(255, 87, 34)
                    else -> Color.rgb(255, 200, 100)
                }
                bg?.setColor(Color.argb(230, 32, 36, 48))
                bg?.setStroke(3, accentColor)
                textView.setTextColor(accentColor)
                textView.paint.isFakeBoldText = true
            } else {
                bg?.setColor(Color.argb(120, 35, 35, 42))
                bg?.setStroke(1, Color.argb(60, 255, 255, 255))
                textView.setTextColor(Color.rgb(200, 205, 215))
                textView.paint.isFakeBoldText = false
            }
        }
    }

    private fun toggleSuperStyleBar() {
        val bar = superStyleBar ?: return
        if (bar.visibility == View.VISIBLE) {
            bar.visibility = View.GONE
        } else {
            bar.visibility = View.VISIBLE
            // 若当前未激活任何超风格，默认激活 16 (正午)
            if (activeSuperStyleId == null || activeSuperStyleId !in 16..18) {
                selectSuperStyle(16)
            }
        }
    }

    /**
     * 开始录制视频与高频传感器数据
     */
    private fun startVideoRecording() {
        if (isRecordingActive.get()) return

        // 检查麦克风权限
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), PERMISSION_REQ_CODE)
        }

        val timeStamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.CHINA).format(Date())
        currentSessionPrefix = "VID_${timeStamp}"

        val dcimDir = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM),
            "AdaptiveGrading"
        ).apply { if (!exists()) mkdirs() }

        val videoFile = File(dcimDir, "${currentSessionPrefix}.mp4")
        currentVideoFile = videoFile
        currentRecordingDir = dcimDir

        // 1. 开启高频陀螺仪监听
        sensorDataLogger.startLogging()

        // 2. 绑定每帧编码元数据采集
        videoRecorder.onFrameEncoded = { frameIndex, timestampNs ->
            val style = getStyleInfoById(engine.getStyle())
            sensorDataLogger.recordFrame(
                frameIndex = frameIndex,
                sensorTimestampNs = timestampNs,
                zoomRatio = currentZoomRatio,
                styleId = style.id,
                styleName = style.name
            )
        }

        // 3. 启动视频编码管线
        videoRecorder.startRecording(videoFile)
        isRecordingActive.set(true)
        recordingDurationSec = 0L

        // 4. 更新顶部计时器状态
        runOnUiThread {
            hudView.isRecording = true
            hudView.recordingDurationSec = 0L
            startRecordingTimer()
            Toast.makeText(this, "🔴 正在录制 (长短焦可缩放，15种风格可随意切换)", Toast.LENGTH_SHORT).show()
        }
    }

    /**
     * 结束录制视频并同步保存陀螺仪 CSV、每帧时间戳 CSV 与相机内参 JSON
     */
    private fun stopVideoRecording() {
        if (!isRecordingActive.getAndSet(false)) return

        stopRecordingTimer()
        runOnUiThread {
            hudView.isRecording = false
            shutterBtn.stopRecordingState()
        }

        // 1. 立即停止传感器记录并直接持久化高频陀螺仪 CSV、每帧时间戳 CSV 与相机内参 JSON
        sensorDataLogger.stopLogging()
        val dir = currentRecordingDir ?: getExternalFilesDir(null) ?: filesDir
        val prefix = currentSessionPrefix
        val (gyroFile, frameFile, intrinsicsFile) = sensorDataLogger.saveRecordingData(dir, prefix, 720, 1280)

        // 2. 冲刷并关闭视频编码器，注册视频至系统媒体库
        videoRecorder.stopRecording {
            currentVideoFile?.let { vFile ->
                registerVideoToMediaStore(vFile)
            }

            runOnUiThread {
                hudView.triggerShutterAnimation("已存入相册: ${currentVideoFile?.name}")
                Toast.makeText(
                    this@CameraActivity,
                    "视频与陀螺仪、内参数据已保存至 DCIM/AdaptiveGrading",
                    Toast.LENGTH_LONG
                ).show()
            }
        }
    }

    private fun startRecordingTimer() {
        stopRecordingTimer()
        timerRunnable = object : Runnable {
            override fun run() {
                if (isRecordingActive.get()) {
                    recordingDurationSec++
                    hudView.recordingDurationSec = recordingDurationSec
                    timerHandler.postDelayed(this, 1000)
                }
            }
        }
        timerHandler.postDelayed(timerRunnable!!, 1000)
    }

    private fun stopRecordingTimer() {
        timerRunnable?.let { timerHandler.removeCallbacks(it) }
        timerRunnable = null
    }

    private fun registerVideoToMediaStore(videoFile: File) {
        if (!videoFile.exists()) return
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, videoFile.name)
            put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(MediaStore.Video.Media.RELATIVE_PATH, "DCIM/AdaptiveGrading")
                put(MediaStore.Video.Media.IS_PENDING, 0)
            }
            put(MediaStore.Video.Media.DATA, videoFile.absolutePath)
        }
        try {
            contentResolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
            MediaScannerConnection.scanFile(this, arrayOf(videoFile.absolutePath), arrayOf("video/mp4"), null)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    /**
     * 拍照并安全捕获 12.5MP 高画质电影调色成片，存入系统 DCIM 相册
     */
    private fun takePhoto() {
        val device = cameraDevice
        val session = captureSession
        val stillReader = stillImageReader

        if (device == null || session == null || stillReader == null || !isCameraFeedActive.get()) {
            // 备用：模拟演示模式或相机未激活时抓取当前调色帧并超采样导出
            pipeline.captureNextGradedFrame { gradedBitmap ->
                val savedFileName = saveBitmapToDCIM(gradedBitmap)
                runOnUiThread {
                    if (savedFileName != null) {
                        hudView.triggerShutterAnimation("已存入相册: $savedFileName")
                        Toast.makeText(this, "照片已保存至系统相册 DCIM/AdaptiveGrading", Toast.LENGTH_SHORT).show()
                    } else {
                        Toast.makeText(this, "拍照保存失败", Toast.LENGTH_SHORT).show()
                    }
                }
            }
            return
        }

        try {
            hudView.triggerShutterAnimation("正在拍摄 12.5MP 高清成片...")
            val captureBuilder = device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE).apply {
                addTarget(stillReader.surface)
                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_HIGH_QUALITY)
                set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_HIGH_QUALITY)
                set(CaptureRequest.JPEG_QUALITY, 100.toByte())
                set(CaptureRequest.JPEG_ORIENTATION, 90)

                // 保持当前变焦倍率
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    set(CaptureRequest.CONTROL_ZOOM_RATIO, currentZoomRatio)
                }
            }
            session.capture(captureBuilder.build(), null, cameraHandler)
        } catch (e: Exception) {
            e.printStackTrace()
            Toast.makeText(this, "拍摄高清照片失败: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    /**
     * 将调色完毕的正立人像照片直接写入系统 DCIM 相册 (Android 10~15 MediaStore Scoped Storage)
     */
    private fun saveBitmapToDCIM(bitmap: Bitmap): String? {
        val timeStamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.CHINA).format(Date())
        val styleId = engine.getStyle()
        val fileName = "ADAPTIVE_${timeStamp}_style${styleId}.jpg"

        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, fileName)
            put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(MediaStore.Images.Media.RELATIVE_PATH, "DCIM/AdaptiveGrading")
                put(MediaStore.Images.Media.IS_PENDING, 1)
            }
        }

        val resolver = contentResolver
        val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
            ?: return null

        try {
            resolver.openOutputStream(uri)?.use { out ->
                bitmap.compress(Bitmap.CompressFormat.JPEG, 98, out)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                values.clear()
                values.put(MediaStore.Images.Media.IS_PENDING, 0)
                resolver.update(uri, values, null, null)
            }
            return fileName
        } catch (e: Exception) {
            e.printStackTrace()
            try { resolver.delete(uri, null, null) } catch (_: Throwable) {}
            return null
        } finally {
            bitmap.recycle()
        }
    }

    /**
     * 导出 Native 512 帧高频环形性能监控日志
     */
    private fun dumpPerformanceLogs() {
        val timeStamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.CHINA).format(Date())
        val logFile = File(
            getExternalFilesDir(null),
            "grading_trace_${timeStamp}.json"
        )
        engine.dumpLogs(logFile.absolutePath)
        Toast.makeText(this, "性能日志已导出至: ${logFile.name}", Toast.LENGTH_LONG).show()
    }

    // --- 相机回调接口实现 ---
    override fun onFrameGraded(
        outputBuffer: ByteBuffer,
        width: Int,
        height: Int,
        metrics: SceneMetrics,
        params: StyleParameters,
        fps: Float,
        latencyMs: Float,
        timestampNs: Long
    ) {
        // 渲染输出到 SurfaceView
        previewView.renderFrame(outputBuffer, width, height)

        // 若当前处于录制状态，投递调色完成帧至硬件视频编码器
        if (isRecordingActive.get()) {
            videoRecorder.feedGradedFrame(outputBuffer, timestampNs)
        }

        // 更新 HUD 监视器状态 (切主线程绘制)
        runOnUiThread {
            hudView.metrics = metrics
            hudView.fps = fps
            hudView.latencyMs = latencyMs
        }
    }

    override fun onAnomalyDetected(message: String, latencyMs: Float) {
        // 性能抖动告警
    }

    // --- Camera2 硬件流对接与管理 ---
    private fun checkAppPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val permissions = mutableListOf(
                Manifest.permission.CAMERA,
                Manifest.permission.RECORD_AUDIO
            )
            val needed = permissions.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
            if (needed.isNotEmpty()) {
                requestPermissions(needed.toTypedArray(), PERMISSION_REQ_CODE)
            } else {
                openCamera2Hardware()
            }
        } else {
            openCamera2Hardware()
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PERMISSION_REQ_CODE) {
            if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                openCamera2Hardware()
            } else {
                Toast.makeText(this, "未授予相机权限，使用实时自适应画质演示模式", Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun openCamera2Hardware() {
        val cameraManager = getSystemService(Context.CAMERA_SERVICE) as? CameraManager ?: return
        try {
            startCameraThread()

            // 查找后置相机
            var targetCameraId: String? = null
            for (id in cameraManager.cameraIdList) {
                val chars = cameraManager.getCameraCharacteristics(id)
                val facing = chars.get(CameraCharacteristics.LENS_FACING)
                if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                    targetCameraId = id
                    // 提取变焦倍率范围及相机内参元数据
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.let { range ->
                            minZoomRatio = range.lower
                            maxZoomRatio = range.upper
                        }
                    }
                    sensorDataLogger.activeCharacteristics = chars
                    break
                }
            }
            if (targetCameraId == null && cameraManager.cameraIdList.isNotEmpty()) {
                targetCameraId = cameraManager.cameraIdList[0]
            }

            if (targetCameraId != null) {
                // 1. 初始化 1280x720 YUV_420_888 实时取景 ImageReader
                imageReader = ImageReader.newInstance(1280, 720, ImageFormat.YUV_420_888, 2).apply {
                    setOnImageAvailableListener({ reader ->
                        val image = reader.acquireLatestImage() ?: return@setOnImageAvailableListener
                        try {
                            isCameraFeedActive.set(true)
                            stopSimulationFeed()

                            val yPlane = image.planes[0]
                            val uPlane = image.planes[1]
                            val vPlane = image.planes[2]

                            val isNv21 = (vPlane.pixelStride == 2)
                            pipeline.processCameraFrame(
                                yPlane = yPlane.buffer,
                                uvPlane = if (isNv21) vPlane.buffer else uPlane.buffer,
                                isNv21 = isNv21,
                                yStride = yPlane.rowStride,
                                uvStride = if (isNv21) vPlane.rowStride else uPlane.rowStride,
                                timestampNs = image.timestamp
                            )
                        } catch (e: Exception) {
                            e.printStackTrace()
                        } finally {
                            image.close()
                        }
                    }, cameraHandler)
                }

                // 2. 初始化 4096x3072 JPEG 高清成片 ImageReader
                stillImageReader = ImageReader.newInstance(4096, 3072, ImageFormat.JPEG, 2).apply {
                    setOnImageAvailableListener({ reader ->
                        val image = reader.acquireLatestImage() ?: return@setOnImageAvailableListener
                        try {
                            val buffer = image.planes[0].buffer
                            val bytes = ByteArray(buffer.remaining())
                            buffer.get(bytes)

                            val decodeOptions = BitmapFactory.Options().apply {
                                inMutable = true
                                inPreferredConfig = Bitmap.Config.ARGB_8888
                            }
                            var bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, decodeOptions)
                            if (bitmap != null) {
                                // 若为横屏原始帧，顺时针旋转 90 度生成 3072x4096 竖屏正立高画质人像照片
                                if (bitmap.width > bitmap.height) {
                                    val rotMatrix = Matrix().apply { postRotate(90f) }
                                    val rotated = Bitmap.createBitmap(
                                        bitmap, 0, 0, bitmap.width, bitmap.height, rotMatrix, true
                                    )
                                    if (rotated != bitmap) {
                                        bitmap.recycle()
                                        bitmap = rotated
                                    }
                                }

                                // 原地调用 C 核心多核 NEON 引擎进行全画质高动态电影级调色
                                engine.gradeBitmap(bitmap)

                                // 写入系统相册
                                val savedFileName = saveBitmapToDCIM(bitmap)
                                runOnUiThread {
                                    if (savedFileName != null) {
                                        hudView.triggerShutterAnimation("已存入相册: $savedFileName (6~8MB)")
                                        Toast.makeText(this@CameraActivity, "高清电影成片已保存至系统相册 ($savedFileName)", Toast.LENGTH_LONG).show()
                                    }
                                }
                            }
                        } catch (e: Exception) {
                            e.printStackTrace()
                        } finally {
                            image.close()
                        }
                    }, cameraHandler)
                }

                if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                    cameraManager.openCamera(targetCameraId, object : CameraDevice.StateCallback() {
                        override fun onOpened(camera: CameraDevice) {
                            cameraDevice = camera
                            createCameraPreviewSession()
                        }

                        override fun onDisconnected(camera: CameraDevice) {
                            camera.close()
                            cameraDevice = null
                            startSimulationFeed()
                        }

                        override fun onError(camera: CameraDevice, error: Int) {
                            camera.close()
                            cameraDevice = null
                            startSimulationFeed()
                        }
                    }, cameraHandler)
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
            startSimulationFeed()
        }
    }

    private fun createCameraPreviewSession() {
        val device = cameraDevice ?: return
        val previewSurface = imageReader?.surface ?: return
        val stillSurface = stillImageReader?.surface ?: return
        try {
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(previewSurface)
                // 自动对焦与曝光
                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)

                // 启用高通 Spectra 硬件 ISP 高画质边缘锐化与去噪
                set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_HIGH_QUALITY)
                set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST)

                // 启用光学防抖 (OIS)
                set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)

                // 保持变焦倍率
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    set(CaptureRequest.CONTROL_ZOOM_RATIO, currentZoomRatio)
                }
            }
            previewRequestBuilder = builder

            device.createCaptureSession(listOf(previewSurface, stillSurface), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    try {
                        session.setRepeatingRequest(builder.build(), captureCallback, cameraHandler)
                    } catch (e: Exception) {
                        e.printStackTrace()
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    startSimulationFeed()
                }
            }, cameraHandler)
        } catch (e: Exception) {
            e.printStackTrace()
            startSimulationFeed()
        }
    }

    private fun closeCamera() {
        isCameraFeedActive.set(false)
        try {
            captureSession?.close()
            captureSession = null
            cameraDevice?.close()
            cameraDevice = null
            previewRequestBuilder = null
            imageReader?.close()
            imageReader = null
            stillImageReader?.close()
            stillImageReader = null
        } catch (e: Exception) {
            e.printStackTrace()
        }
        stopCameraThread()
    }

    private fun startCameraThread() {
        if (cameraThread == null) {
            cameraThread = HandlerThread("CameraBackground").apply {
                start()
                cameraHandler = Handler(looper)
            }
        }
    }

    private fun stopCameraThread() {
        cameraThread?.quitSafely()
        try {
            cameraThread?.join()
            cameraThread = null
            cameraHandler = null
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    // --- 实时高帧率演示画面发生器 ---
    private fun startSimulationFeed() {
        if (isSimulationRunning.get() || isCameraFeedActive.get()) return
        isSimulationRunning.set(true)

        val w = 1280
        val h = 720
        val ySize = w * h
        val uvSize = w * h / 2

        val yBuf = ByteBuffer.allocateDirect(ySize).order(ByteOrder.nativeOrder())
        val uvBuf = ByteBuffer.allocateDirect(uvSize).order(ByteOrder.nativeOrder())

        simulationThread = Thread {
            var phase = 0.0f
            while (isSimulationRunning.get() && !isCameraFeedActive.get()) {
                val t0 = SystemClock.elapsedRealtime()
                phase += 0.04f

                // 生成动态渐变日落 / 人像肤色 / 霓虹风景测试信号
                yBuf.position(0)
                uvBuf.position(0)
                val cosP = Math.cos(phase.toDouble()).toFloat()

                val yBase = ((128 + 60 * cosP).toInt()).toByte()
                for (i in 0 until ySize step 16) {
                    yBuf.put(yBase)
                    yBuf.put((yBase + 10).toByte())
                    yBuf.put((yBase - 10).toByte())
                    yBuf.put(yBase)
                    yBuf.put((yBase + 20).toByte())
                    yBuf.put(yBase)
                    yBuf.put((yBase - 5).toByte())
                    yBuf.put(yBase)
                    yBuf.put((yBase + 15).toByte())
                    yBuf.put((yBase - 15).toByte())
                    yBuf.put(yBase)
                    yBuf.put(yBase)
                    yBuf.put(yBase)
                    yBuf.put(yBase)
                    yBuf.put(yBase)
                    yBuf.put(yBase)
                }

                val uVal = ((115 + 30 * Math.sin(phase.toDouble())).toInt()).toByte()
                val vVal = ((150 + 20 * Math.cos(phase.toDouble())).toInt()).toByte()
                for (i in 0 until uvSize step 2) {
                    uvBuf.put(vVal)
                    uvBuf.put(uVal)
                }

                yBuf.position(0)
                uvBuf.position(0)

                try {
                    pipeline.processCameraFrame(
                        yPlane = yBuf,
                        uvPlane = uvBuf,
                        isNv21 = true,
                        yStride = w,
                        uvStride = w,
                        timestampNs = SystemClock.elapsedRealtimeNanos()
                    )
                } catch (e: Throwable) {
                    android.util.Log.e("SimFeed", "Frame process error", e)
                }

                val elapsed = SystemClock.elapsedRealtime() - t0
                val sleepMs = 16L - elapsed
                if (sleepMs > 0) {
                    try { Thread.sleep(sleepMs) } catch (e: InterruptedException) { break }
                }
            }
        }.apply {
            name = "SimulationGradingFeed"
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun stopSimulationFeed() {
        isSimulationRunning.set(false)
        simulationThread?.interrupt()
        simulationThread = null
    }

    override fun onResume() {
        super.onResume()
        if (::pipeline.isInitialized) {
            pipeline.start()
        }
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            openCamera2Hardware()
        } else {
            startSimulationFeed()
        }
    }

    override fun onPause() {
        super.onPause()
        if (isRecordingActive.get()) {
            stopVideoRecording()
        }
        if (::pipeline.isInitialized) {
            pipeline.stop()
        }
        stopSimulationFeed()
        closeCamera()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isRecordingActive.get()) {
            stopVideoRecording()
        }
        stopSimulationFeed()
        closeCamera()
        if (::pipeline.isInitialized) {
            pipeline.close()
        }
        if (::engine.isInitialized) {
            engine.close()
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            setImmersiveFullScreen()
        }
    }

    private fun setImmersiveFullScreen() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val controller = window?.decorView?.windowInsetsController ?: window?.insetsController
                controller?.let {
                    it.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                    it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                }
            } else {
                @Suppress("DEPRECATION")
                window?.decorView?.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                )
            }
        } catch (_: Throwable) {
            // 防御性保护
        }
    }
}
