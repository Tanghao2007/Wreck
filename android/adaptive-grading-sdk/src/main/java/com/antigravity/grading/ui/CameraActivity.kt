package com.antigravity.grading.ui

import android.Manifest
import android.app.Activity
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
import android.os.*
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Toast
import com.antigravity.grading.AdaptiveGradingEngine
import com.antigravity.grading.CameraGradingListener
import com.antigravity.grading.CameraPipeline
import com.antigravity.grading.STYLE_CATALOG
import com.antigravity.grading.SceneMetrics
import com.antigravity.grading.StyleParameters
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 实时自适应电影调色相机主界面 Activity
 * 实现了完整的相机生命周期、权限控制、双流渲染、分屏对比与配方导出
 */
class CameraActivity : Activity(), CameraGradingListener {

    private lateinit var engine: AdaptiveGradingEngine
    private lateinit var pipeline: CameraPipeline

    private lateinit var previewView: CameraPreviewView
    private lateinit var hudView: HUDOverlayView
    private lateinit var splitSlider: SplitScreenSlider
    private lateinit var styleSelector: StyleSelectorView

    // Camera2 硬件流组件
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var stillImageReader: ImageReader? = null
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private val isCameraFeedActive = AtomicBoolean(false)

    // 备用实时渲染发生器 (相机未就绪或未授权时保证界面 60FPS 实时可交互)
    private var simulationThread: Thread? = null
    private val isSimulationRunning = AtomicBoolean(false)

    private val CAMERA_PERMISSION_REQ_CODE = 1001

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 1. 初始化 C 调色引擎 (默认启动风格 1: 自然人像)
        try {
            engine = AdaptiveGradingEngine(defaultStyleId = 1)
            pipeline = CameraPipeline(this, engine, previewWidth = 1280, previewHeight = 720)
            pipeline.listener = this
        } catch (e: Throwable) {
            android.util.Log.e("CameraActivity", "调色引擎初始化异常", e)
            Toast.makeText(this, "调色引擎初始化失败: ${e.message}", Toast.LENGTH_LONG).show()
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

        // 中层: 分屏对比拖拽中线 (默认隐藏，开启分屏模式时显示)
        splitSlider = SplitScreenSlider(this).apply {
            visibility = View.GONE
            listener = { ratio ->
                pipeline.splitRatio = ratio
            }
        }
        rootLayout.addView(splitSlider, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)

        // 顶层: OSD 状态与信息胶囊覆盖层
        hudView = HUDOverlayView(this).apply {
            currentStyle = STYLE_CATALOG[0]
        }
        rootLayout.addView(hudView, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)

        // 浮层控制栏: 顶部操作按钮 (分屏对比切换、HUD 显示切换、日志导出)
        val topBar = createTopBar()
        rootLayout.addView(
            topBar,
            FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.TOP
                topMargin = 170
            }
        )

        // 底部控制底栏: 风格选择横向轮播条 + 快门按键
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
        checkCameraPermissions()
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

            // 弹性间隔
            addView(View(context), LinearLayout.LayoutParams(0, 1, 1f))

            // 2. HUD 仪表盘开关按钮
            val hudBtn = createPillButton("仪表盘") {
                hudView.isVisibleHUD = !hudView.isVisibleHUD
            }
            addView(hudBtn)

            // 3. 日志导出按钮
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

            // 1. 15 款电影风格轮播器
            styleSelector = StyleSelectorView(context).apply {
                onStyleSelected = { style ->
                    engine.setStyle(style.id)
                    hudView.currentStyle = style
                }
            }
            addView(
                styleSelector,
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            )

            // 2. 拍摄快门控制区域
            val shutterArea = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER
                setPadding(0, 30, 0, 10)

                // 快门按键
                val shutterBtn = createShutterButton {
                    takePhoto()
                }
                addView(shutterBtn)
            }
            addView(shutterArea)
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

    private fun createShutterButton(onClick: () -> Unit): View {
        val outerSize = 180
        return FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(outerSize, outerSize)
            isClickable = true
            isFocusable = true
            setOnClickListener { onClick() }

            // 外白圈
            val outerRing = View(context).apply {
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(Color.TRANSPARENT)
                    setStroke(8, Color.WHITE)
                }
            }
            addView(outerRing, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)

            // 内实心白圆 (带微小间距)
            val innerSize = outerSize - 32
            val innerCircle = View(context).apply {
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(Color.WHITE)
                }
            }
            val innerLp = FrameLayout.LayoutParams(innerSize, innerSize).apply {
                gravity = Gravity.CENTER
            }
            addView(innerCircle, innerLp)
        }
    }

    /**
     * 拍照并安全捕获 12.5MP 高画质 (4096x3072 / 6-8MB) 电影调色成片，存入系统 DCIM 相册
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
    private fun saveBitmapToDCIM(bitmap: android.graphics.Bitmap): String? {
        val timeStamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.CHINA).format(Date())
        val styleId = engine.getStyle()
        val fileName = "ADAPTIVE_${timeStamp}_style${styleId}.jpg"

        val values = android.content.ContentValues().apply {
            put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, fileName)
            put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(android.provider.MediaStore.Images.Media.RELATIVE_PATH, "DCIM/AdaptiveGrading")
                put(android.provider.MediaStore.Images.Media.IS_PENDING, 1)
            }
        }

        val resolver = contentResolver
        val uri = resolver.insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
            ?: return null

        try {
            resolver.openOutputStream(uri)?.use { out ->
                bitmap.compress(android.graphics.Bitmap.CompressFormat.JPEG, 98, out)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                values.clear()
                values.put(android.provider.MediaStore.Images.Media.IS_PENDING, 0)
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
        latencyMs: Float
    ) {
        // 渲染输出到 SurfaceView
        previewView.renderFrame(outputBuffer, width, height)

        // 更新 HUD 监视器状态 (切主线程绘制)
        runOnUiThread {
            hudView.metrics = metrics
            hudView.fps = fps
            hudView.latencyMs = latencyMs
        }
    }

    override fun onAnomalyDetected(message: String, latencyMs: Float) {
        // 当发生性能抖动或时延超限时被调用
    }

    // --- Camera2 硬件流对接与管理 ---
    private fun checkCameraPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val permissions = arrayOf(Manifest.permission.CAMERA)
            val needed = permissions.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
            if (needed.isNotEmpty()) {
                requestPermissions(needed.toTypedArray(), CAMERA_PERMISSION_REQ_CODE)
            } else {
                openCamera2Hardware()
            }
        } else {
            openCamera2Hardware()
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == CAMERA_PERMISSION_REQ_CODE) {
            if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
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
                    break
                }
            }
            if (targetCameraId == null && cameraManager.cameraIdList.isNotEmpty()) {
                targetCameraId = cameraManager.cameraIdList[0]
            }

            if (targetCameraId != null) {
                // 1. 初始化 1280x720 YUV_420_888 实时取景 ImageReader (保证 30~60 FPS 满帧流)
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
                                uvStride = if (isNv21) vPlane.rowStride else uPlane.rowStride
                            )
                        } catch (e: Exception) {
                            e.printStackTrace()
                        } finally {
                            image.close()
                        }
                    }, cameraHandler)
                }

                // 2. 初始化 4096x3072 JPEG 高清成片 ImageReader (OnePlus 13 50MP 四合一 12.5MP 原生超清成片，产物体积 6~8MB)
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

                                // 写入系统 DCIM/AdaptiveGrading 媒体库，质量 98 (产物体积 6~8MB)
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
            val previewRequestBuilder = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(previewSurface)
                // 自动对焦与曝光
                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)

                // 启用高通 Spectra 硬件 ISP 高画质边缘锐化与去噪 (消除原画面偏糊现象)
                set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_HIGH_QUALITY)
                set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST)

                // 启用光学防抖 (OIS) 避免手持微抖
                set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
            }

            device.createCaptureSession(listOf(previewSurface, stillSurface), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    try {
                        session.setRepeatingRequest(previewRequestBuilder.build(), null, cameraHandler)
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
                        uvStride = w
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
        if (::pipeline.isInitialized) {
            pipeline.stop()
        }
        stopSimulationFeed()
        closeCamera()
    }

    override fun onDestroy() {
        super.onDestroy()
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
            // 防御性保护，避免 Android 15 / 各厂商定制系统 DecorView 初始化竞争
        }
    }
}

