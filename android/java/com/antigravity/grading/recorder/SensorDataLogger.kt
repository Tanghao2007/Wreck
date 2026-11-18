package com.antigravity.grading.recorder

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CaptureResult
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Environment
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.FileWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentLinkedQueue

/**
 * 手机陀螺仪原始数据采样点
 */
data class GyroSample(
    val timestampNs: Long,
    val utcMs: Long,
    val gx: Float,
    val gy: Float,
    val gz: Float,
    val uncalibX: Float = 0f,
    val uncalibY: Float = 0f,
    val uncalibZ: Float = 0f,
    val biasX: Float = 0f,
    val biasY: Float = 0f,
    val biasZ: Float = 0f
)

/**
 * 每帧相机状态与时间戳采样点
 */
data class FrameSample(
    val frameIndex: Long,
    val sensorTimestampNs: Long,
    val systemTimeNs: Long,
    val utcIso: String,
    val exposureTimeNs: Long,
    val iso: Int,
    val focalLengthMm: Float,
    val focusDistance: Float,
    val zoomRatio: Float,
    val styleId: Int,
    val styleName: String
)

/**
 * 高频传感器、每帧时间戳与相机内参记录器
 * 负责高精度陀螺仪流式捕获、每帧元数据同步及标准化 CSV/JSON 导出
 */
class SensorDataLogger(private val context: Context) : SensorEventListener {

    private val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val gyroSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val uncalibGyroSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE_UNCALIBRATED)

    private val gyroQueue = ConcurrentLinkedQueue<GyroSample>()
    private val frameQueue = ConcurrentLinkedQueue<FrameSample>()

    @Volatile
    private var isRecording = false

    private var latestUncalibX = 0f
    private var latestUncalibY = 0f
    private var latestUncalibZ = 0f
    private var latestBiasX = 0f
    private var latestBiasY = 0f
    private var latestBiasZ = 0f

    // 活跃相机参数缓存
    @Volatile
    var activeCharacteristics: CameraCharacteristics? = null

    @Volatile
    var currentCaptureResult: CaptureResult? = null

    /**
     * 开启高频陀螺仪监听
     */
    fun startLogging() {
        gyroQueue.clear()
        frameQueue.clear()
        isRecording = true

        sensorManager?.let { sm ->
            // 以最高速率 (SENSOR_DELAY_FASTEST) 注册校准陀螺仪与未校准陀螺仪
            gyroSensor?.let { sensor ->
                sm.registerListener(this, sensor, SensorManager.SENSOR_DELAY_FASTEST)
            }
            uncalibGyroSensor?.let { sensor ->
                sm.registerListener(this, sensor, SensorManager.SENSOR_DELAY_FASTEST)
            }
        }
    }

    /**
     * 停止高频陀螺仪监听
     */
    fun stopLogging() {
        isRecording = false
        sensorManager?.unregisterListener(this)
    }

    override fun onSensorChanged(event: SensorEvent?) {
        if (!isRecording || event == null) return

        val nowUtc = System.currentTimeMillis()
        val ts = event.timestamp

        when (event.sensor.type) {
            Sensor.TYPE_GYROSCOPE_UNCALIBRATED -> {
                if (event.values.size >= 6) {
                    latestUncalibX = event.values[0]
                    latestUncalibY = event.values[1]
                    latestUncalibZ = event.values[2]
                    latestBiasX = event.values[3]
                    latestBiasY = event.values[4]
                    latestBiasZ = event.values[5]
                }
            }
            Sensor.TYPE_GYROSCOPE -> {
                val gx = event.values[0]
                val gy = event.values[1]
                val gz = event.values[2]
                gyroQueue.add(
                    GyroSample(
                        timestampNs = ts,
                        utcMs = nowUtc,
                        gx = gx,
                        gy = gy,
                        gz = gz,
                        uncalibX = latestUncalibX,
                        uncalibY = latestUncalibY,
                        uncalibZ = latestUncalibZ,
                        biasX = latestBiasX,
                        biasY = latestBiasY,
                        biasZ = latestBiasZ
                    )
                )
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    /**
     * 记录一帧视频编码时的元数据与时间戳
     */
    fun recordFrame(
        frameIndex: Long,
        sensorTimestampNs: Long,
        zoomRatio: Float,
        styleId: Int,
        styleName: String
    ) {
        if (!isRecording) return

        val result = currentCaptureResult
        val exposureTimeNs = result?.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: 0L
        val iso = result?.get(CaptureResult.SENSOR_SENSITIVITY) ?: 0
        val focalLength = result?.get(CaptureResult.LENS_FOCAL_LENGTH) ?: 0f
        val focusDistance = result?.get(CaptureResult.LENS_FOCUS_DISTANCE) ?: 0f

        val isoFormat = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US)
        val utcIso = isoFormat.format(Date())

        frameQueue.add(
            FrameSample(
                frameIndex = frameIndex,
                sensorTimestampNs = sensorTimestampNs,
                systemTimeNs = SystemClock.elapsedRealtimeNanos(),
                utcIso = utcIso,
                exposureTimeNs = exposureTimeNs,
                iso = iso,
                focalLengthMm = focalLength,
                focusDistance = focusDistance,
                zoomRatio = zoomRatio,
                styleId = styleId,
                styleName = styleName
            )
        )
    }

    /**
     * 将录制会话捕获的所有数据落盘为标准 CSV 和 JSON 文件
     * 多路径冗余持久化：
     * 1. baseDir (例如 /sdcard/DCIM/AdaptiveGrading/) 与视频成片并排存放
     * 2. appPrivateDir (/sdcard/Android/data/com.antigravity.grading/files/Recordings/) 100% 权限可用保障区
     * 3. downloadsDir (/sdcard/Download/AdaptiveGrading/) 方便用户手机文件浏览器直接查看
     */
    fun saveRecordingData(
        baseDir: File,
        sessionPrefix: String,
        videoWidth: Int,
        videoHeight: Int
    ): Triple<File, File, File> {
        // 1. 构建完整的内存内容，保证无论写入哪份介质数据均完整不丢失
        val gyroSb = StringBuilder()
        gyroSb.append("timestamp_ns,utc_ms,gyro_x_rad_s,gyro_y_rad_s,gyro_z_rad_s,uncalib_x,uncalib_y,uncalib_z,bias_x,bias_y,bias_z\n")
        var gyroCount = 0
        while (gyroQueue.isNotEmpty()) {
            val s = gyroQueue.poll() ?: break
            gyroSb.append("${s.timestampNs},${s.utcMs},${s.gx},${s.gy},${s.gz},${s.uncalibX},${s.uncalibY},${s.uncalibZ},${s.biasX},${s.biasY},${s.biasZ}\n")
            gyroCount++
        }
        val gyroContent = gyroSb.toString()

        val frameSb = StringBuilder()
        frameSb.append("frame_index,sensor_timestamp_ns,system_time_ns,utc_iso,exposure_time_ns,iso,focal_length_mm,focus_distance,zoom_ratio,style_id,style_name\n")
        var frameCount = 0
        while (frameQueue.isNotEmpty()) {
            val f = frameQueue.poll() ?: break
            frameSb.append("${f.frameIndex},${f.sensorTimestampNs},${f.systemTimeNs},\"${f.utcIso}\",${f.exposureTimeNs},${f.iso},${f.focalLengthMm},${f.focusDistance},${f.zoomRatio},${f.styleId},\"${f.styleName}\"\n")
            frameCount++
        }
        val frameContent = frameSb.toString()

        val intrinsicsContent = buildCameraIntrinsicsJson(videoWidth, videoHeight)
        Log.i("SensorDataLogger", "Prepared log data: $gyroCount gyro samples, $frameCount frames recorded")

        // 2. 准备输出目标目录列表
        val appPrivateDir = File(context.getExternalFilesDir(null), "Recordings").apply { mkdirs() }
        val downloadsDir = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "AdaptiveGrading"
        ).apply { mkdirs() }

        val targetDirs = listOf(baseDir, appPrivateDir, downloadsDir)

        var finalGyroFile = File(appPrivateDir, "${sessionPrefix}_gyroscope.csv")
        var finalFrameFile = File(appPrivateDir, "${sessionPrefix}_frames.csv")
        var finalIntrinsicsFile = File(appPrivateDir, "${sessionPrefix}_camera_intrinsics.json")

        for (dir in targetDirs) {
            try {
                if (!dir.exists()) dir.mkdirs()
                val gFile = File(dir, "${sessionPrefix}_gyroscope.csv")
                val fFile = File(dir, "${sessionPrefix}_frames.csv")
                val iFile = File(dir, "${sessionPrefix}_camera_intrinsics.json")

                gFile.writeText(gyroContent)
                fFile.writeText(frameContent)
                iFile.writeText(intrinsicsContent)

                MediaScannerConnection.scanFile(
                    context,
                    arrayOf(gFile.absolutePath, fFile.absolutePath, iFile.absolutePath),
                    null,
                    null
                )
                Log.i("SensorDataLogger", "Successfully wrote session data to: ${dir.absolutePath} (gyro: ${gFile.length()}B, frames: ${fFile.length()}B, intrinsics: ${iFile.length()}B)")

                if (dir == baseDir) {
                    finalGyroFile = gFile
                    finalFrameFile = fFile
                    finalIntrinsicsFile = iFile
                }
            } catch (e: Exception) {
                Log.w("SensorDataLogger", "Could not write to ${dir.absolutePath}: ${e.message}")
            }
        }

        return Triple(finalGyroFile, finalFrameFile, finalIntrinsicsFile)
    }

    /**
     * 读取 OnePlus 13 硬件内参并构造标准 JSON 描述
     */
    private fun buildCameraIntrinsicsJson(videoWidth: Int, videoHeight: Int): String {
        val chars = activeCharacteristics

        // 原始传感器内参 [fx, fy, cx, cy, skew]
        val rawCalibration = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            chars?.get(CameraCharacteristics.LENS_INTRINSIC_CALIBRATION)
        } else null

        // 镜头畸变系数 [k1, k2, k3, k4, k5]
        val rawDistortion = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            chars?.get(CameraCharacteristics.LENS_DISTORTION)
        } else null

        val pixelArray = chars?.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
        val activeArray = chars?.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
        val physicalSize = chars?.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
        val focalLengths = chars?.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
        val zoomRange = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            chars?.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
        } else null

        val activeW = activeArray?.width() ?: (pixelArray?.width ?: 4096)
        val activeH = activeArray?.height() ?: (pixelArray?.height ?: 3072)

        val fxRaw = rawCalibration?.getOrNull(0) ?: (activeW * 0.7f)
        val fyRaw = rawCalibration?.getOrNull(1) ?: (activeH * 0.7f)
        val cxRaw = rawCalibration?.getOrNull(2) ?: (activeW / 2.0f)
        val cyRaw = rawCalibration?.getOrNull(3) ?: (activeH / 2.0f)
        val skew = rawCalibration?.getOrNull(4) ?: 0.0f

        // 换算到 720x1280 竖屏录制分辨率的缩放内参
        val scaleX = videoWidth.toFloat() / activeH.toFloat()
        val scaleY = videoHeight.toFloat() / activeW.toFloat()

        val fxVideo = fyRaw * scaleX
        val fyVideo = fxRaw * scaleY
        val cxVideo = cyRaw * scaleX
        val cyVideo = (activeW - cxRaw) * scaleY

        val calibArrayStr = rawCalibration?.joinToString(prefix = "[", postfix = "]") ?: "[]"
        val distArrayStr = rawDistortion?.joinToString(prefix = "[", postfix = "]") ?: "[]"
        val focalArrayStr = focalLengths?.joinToString(prefix = "[", postfix = "]") ?: "[]"
        val zoomRangeStr = if (zoomRange != null) "[${zoomRange.lower}, ${zoomRange.upper}]" else "[1.0, 10.0]"

        return """
        {
          "device": "${Build.MANUFACTURER} ${Build.MODEL}",
          "os_version": "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
          "video_format": {
            "width": $videoWidth,
            "height": $videoHeight,
            "orientation": "portrait_90deg"
          },
          "sensor_specs": {
            "pixel_array_size": [${pixelArray?.width ?: 4096}, ${pixelArray?.height ?: 3072}],
            "active_array_size": [${activeArray?.left ?: 0}, ${activeArray?.top ?: 0}, ${activeArray?.right ?: 4096}, ${activeArray?.bottom ?: 3072}],
            "physical_size_mm": [${physicalSize?.width ?: 8.0f}, ${physicalSize?.height ?: 6.0f}],
            "focal_lengths_available_mm": $focalArrayStr,
            "zoom_ratio_range": $zoomRangeStr
          },
          "lens_intrinsic_calibration_raw": $calibArrayStr,
          "lens_distortion_raw": $distArrayStr,
          "video_scaled_intrinsics": {
            "fx": $fxVideo,
            "fy": $fyVideo,
            "cx": $cxVideo,
            "cy": $cyVideo,
            "skew": $skew,
            "matrix_k": [
              [$fxVideo, $skew, $cxVideo],
              [0.0, $fyVideo, $cyVideo],
              [0.0, 0.0, 1.0]
            ]
          }
        }
        """.trimIndent()
    }
}
