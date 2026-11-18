package com.antigravity.grading.recorder

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.*
import android.opengl.*
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.Surface
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 实时自适应电影调色视频编码器
 * 采用硬件 MediaCodec (H.264 / AVC + AAC) + OpenGL ES 纹理旋转管线 + MediaMuxer
 * 直通 Adreno 硬件编解码器，单帧渲染耗时 < 1ms，稳定输出 720x1280 竖屏电影视频。
 */
class VideoRecorder(
    private val context: Context,
    val videoWidth: Int = 720,
    val videoHeight: Int = 1280,
    val frameWidth: Int = 1280,
    val frameHeight: Int = 720,
    val bitRate: Int = 10_000_000, // 10 Mbps 高清画质
    val frameRate: Int = 30
) {
    companion object {
        private const val TAG = "VideoRecorder"
        private const val VIDEO_MIME = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val AUDIO_MIME = MediaFormat.MIMETYPE_AUDIO_AAC
        private const val EGL_RECORDABLE_ANDROID = 0x3142
    }

    private val isRecording = AtomicBoolean(false)
    private var outputFile: File? = null

    // 录制后台处理线程
    private var encoderThread: HandlerThread? = null
    private var encoderHandler: Handler? = null

    // 音频采集线程
    private var audioThread: Thread? = null
    private val isAudioRunning = AtomicBoolean(false)

    // Media 编解码与封装器
    private var videoCodec: MediaCodec? = null
    private var audioCodec: MediaCodec? = null
    private var mediaMuxer: MediaMuxer? = null
    private var videoTrackIndex = -1
    private var audioTrackIndex = -1
    private var isMuxerStarted = false
    private val muxerLock = Object()

    // OpenGL ES & EGL 环境
    private var inputSurface: Surface? = null
    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var glProgram = 0
    private var textureId = 0
    private var vertexBuffer: FloatBuffer? = null
    private var texCoordBuffer: FloatBuffer? = null

    private var firstFrameTimeNs: Long = -1L
    private var frameCounter: Long = 0L

    var onFrameEncoded: ((frameIndex: Long, timestampNs: Long) -> Unit)? = null

    /**
     * 开始录制视频
     */
    fun startRecording(destinationFile: File) {
        if (isRecording.get()) return
        outputFile = destinationFile
        isRecording.set(true)
        firstFrameTimeNs = -1L
        frameCounter = 0L
        videoTrackIndex = -1
        audioTrackIndex = -1
        isMuxerStarted = false

        // 1. 初始化编码线程
        encoderThread = HandlerThread("VideoEncoderThread", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply {
            start()
            encoderHandler = Handler(looper)
        }

        encoderHandler?.post {
            try {
                // 初始化 MediaMuxer
                mediaMuxer = MediaMuxer(destinationFile.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)

                // 初始化视频编码器与 EGL 环境
                setupVideoEncoder()
                setupEGL()
                setupGLShaders()

                // 若有麦克风权限，启动音频录制
                if (hasAudioPermission()) {
                    setupAudioEncoder()
                    startAudioCaptureThread()
                } else {
                    Log.w(TAG, "Audio permission not granted, recording video only.")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start recording", e)
                stopRecording()
            }
        }
    }

    /**
     * 投递一帧 RGBA (1280x720) 调色帧给编码管线
     */
    fun feedGradedFrame(rgbaBuffer: ByteBuffer, timestampNs: Long) {
        if (!isRecording.get()) return

        // 拷贝 DirectBuffer 避免与渲染/相机流竞态
        val bufferCopy = ByteBuffer.allocateDirect(rgbaBuffer.capacity()).order(ByteOrder.nativeOrder())
        rgbaBuffer.position(0)
        bufferCopy.put(rgbaBuffer)
        rgbaBuffer.position(0)
        bufferCopy.position(0)

        encoderHandler?.post {
            if (!isRecording.get()) return@post
            try {
                if (firstFrameTimeNs < 0) {
                    firstFrameTimeNs = timestampNs
                }
                val presentationTimeNs = timestampNs - firstFrameTimeNs

                // 1. 渲染纹理到 Surface
                renderToEncoderSurface(bufferCopy)

                // 2. 赋予硬件时间戳并交换缓冲区
                EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, presentationTimeNs)
                EGL14.eglSwapBuffers(eglDisplay, eglSurface)

                // 3. 抽取视频编码器输出
                drainVideoEncoder(false)

                frameCounter++
                onFrameEncoded?.invoke(frameCounter, timestampNs)
            } catch (e: Exception) {
                Log.e(TAG, "Error encoding frame", e)
            }
        }
    }

    /**
     * 停止录制并落盘释放资源
     */
    fun stopRecording(onComplete: (() -> Unit)? = null) {
        if (!isRecording.getAndSet(false)) {
            onComplete?.invoke()
            return
        }

        encoderHandler?.post {
            try {
                // 1. 停止音频采集并投递音频 EOS
                isAudioRunning.set(false)
                try {
                    audioThread?.interrupt()
                    audioThread?.join(300)
                } catch (_: Exception) {}
                audioThread = null

                audioCodec?.let { codec ->
                    try {
                        val inputBufIdx = codec.dequeueInputBuffer(10_000)
                        if (inputBufIdx >= 0) {
                            codec.queueInputBuffer(inputBufIdx, 0, 0, 0L, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        }
                    } catch (_: Exception) {}
                }
                drainAudioEncoder(true)

                // 2. 发送视频流结束标识 (EOS) 并冲刷剩余帧
                try {
                    videoCodec?.signalEndOfInputStream()
                } catch (_: Exception) {}
                drainVideoEncoder(true)

                // 3. 释放所有底层硬件资源
                releaseAll()
                Log.i(TAG, "Video recording stopped successfully: ${outputFile?.absolutePath}")
            } catch (e: Exception) {
                Log.e(TAG, "Error during stopRecording", e)
            } finally {
                encoderThread?.quitSafely()
                encoderThread = null
                encoderHandler = null
                onComplete?.invoke()
            }
        }
    }

    private fun hasAudioPermission(): Boolean {
        return context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
    }

    private fun setupVideoEncoder() {
        val format = MediaFormat.createVideoFormat(VIDEO_MIME, videoWidth, videoHeight).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitRate)
            setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1) // 1 秒 1 个关键帧，保证剪辑秒开
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        }

        videoCodec = MediaCodec.createEncoderByType(VIDEO_MIME).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            inputSurface = createInputSurface()
            start()
        }
    }

    private fun setupAudioEncoder() {
        val format = MediaFormat.createAudioFormat(AUDIO_MIME, 44100, 1).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, 128_000)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
        }

        audioCodec = MediaCodec.createEncoderByType(AUDIO_MIME).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            start()
        }
    }

    private fun startAudioCaptureThread() {
        val sampleRate = 44100
        val channelConfig = AudioFormat.CHANNEL_IN_MONO
        val audioFormat = AudioFormat.ENCODING_PCM_16BIT
        val minBufferSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)
        val bufferSize = maxOf(minBufferSize * 2, 4096)

        val audioRecord = try {
            AudioRecord(
                MediaRecorder.AudioSource.CAMCORDER,
                sampleRate,
                channelConfig,
                audioFormat,
                bufferSize
            )
        } catch (e: SecurityException) {
            Log.e(TAG, "AudioRecord security exception", e)
            return
        }

        if (audioRecord.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord state not initialized")
            audioRecord.release()
            return
        }

        audioRecord.startRecording()
        isAudioRunning.set(true)

        audioThread = Thread {
            val audioBuffer = ByteArray(2048)
            val codec = audioCodec ?: return@Thread

            while (isAudioRunning.get()) {
                val readBytes = audioRecord.read(audioBuffer, 0, audioBuffer.size)
                if (readBytes > 0) {
                    val inputBufIdx = codec.dequeueInputBuffer(10_000)
                    if (inputBufIdx >= 0) {
                        val inputBuf = codec.getInputBuffer(inputBufIdx)
                        inputBuf?.clear()
                        inputBuf?.put(audioBuffer, 0, readBytes)
                        val ptsNs = System.nanoTime()
                        val ptsUs = if (firstFrameTimeNs > 0) (ptsNs - firstFrameTimeNs) / 1000 else 0L
                        codec.queueInputBuffer(inputBufIdx, 0, readBytes, ptsUs, 0)
                    }
                    drainAudioEncoder(false)
                }
            }

            try {
                audioRecord.stop()
                audioRecord.release()
            } catch (_: Exception) {}
        }.apply {
            name = "AudioRecordThread"
            priority = Thread.NORM_PRIORITY
            start()
        }
    }

    private fun setupEGL() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val version = IntArray(2)
        EGL14.eglInitialize(eglDisplay, version, 0, version, 1)

        val configAttribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL_RECORDABLE_ANDROID, 1,
            EGL14.EGL_NONE
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val numConfigs = IntArray(1)
        EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfigs, 0)
        val eglConfig = configs[0] ?: throw RuntimeException("Failed to choose EGL config")

        val contextAttribs = intArrayOf(
            EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL14.EGL_NONE
        )
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT, contextAttribs, 0)

        val surfaceAttribs = intArrayOf(EGL14.EGL_NONE)
        val surface = inputSurface ?: throw RuntimeException("InputSurface is null")
        eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, surface, surfaceAttribs, 0)

        EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)
    }

    private fun setupGLShaders() {
        val vertexShaderCode = """
            attribute vec4 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = aTexCoord;
            }
        """.trimIndent()

        val fragmentShaderCode = """
            precision mediump float;
            varying vec2 vTexCoord;
            uniform sampler2D uTexture;
            void main() {
                gl_FragColor = texture2D(uTexture, vTexCoord);
            }
        """.trimIndent()

        val vs = loadShader(GLES20.GL_VERTEX_SHADER, vertexShaderCode)
        val fs = loadShader(GLES20.GL_FRAGMENT_SHADER, fragmentShaderCode)

        glProgram = GLES20.glCreateProgram().also {
            GLES20.glAttachShader(it, vs)
            GLES20.glAttachShader(it, fs)
            GLES20.glLinkProgram(it)
        }

        // 全屏四边形顶点 NDC [-1, 1]
        val quadCoords = floatArrayOf(
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f
        )
        vertexBuffer = ByteBuffer.allocateDirect(quadCoords.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
            put(quadCoords)
            position(0)
        }

        // 纹理坐标映射：将横屏 1280x720 顺时针旋转 90 度填充至 720x1280 竖屏画布
        val texCoords = floatArrayOf(
            1.0f, 1.0f, // 对应顶点 (-1, -1) 左下
            1.0f, 0.0f, // 对应顶点 ( 1, -1) 右下
            0.0f, 1.0f, // 对应顶点 (-1,  1) 左上
            0.0f, 0.0f  // 对应顶点 ( 1,  1) 右上
        )
        texCoordBuffer = ByteBuffer.allocateDirect(texCoords.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
            put(texCoords)
            position(0)
        }

        // 生成 2D 纹理句柄
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        textureId = textures[0]
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
    }

    private fun renderToEncoderSurface(rgbaBuffer: ByteBuffer) {
        GLES20.glViewport(0, 0, videoWidth, videoHeight)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glUseProgram(glProgram)

        // 上传 1280x720 RGBA 帧像素数据到 GPU 纹理
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
        rgbaBuffer.position(0)
        GLES20.glTexImage2D(
            GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA,
            frameWidth, frameHeight, 0,
            GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgbaBuffer
        )

        val uTextureHandle = GLES20.glGetUniformLocation(glProgram, "uTexture")
        GLES20.glUniform1i(uTextureHandle, 0)

        val aPosHandle = GLES20.glGetAttribLocation(glProgram, "aPosition")
        GLES20.glEnableVertexAttribArray(aPosHandle)
        GLES20.glVertexAttribPointer(aPosHandle, 2, GLES20.GL_FLOAT, false, 0, vertexBuffer)

        val aTexHandle = GLES20.glGetAttribLocation(glProgram, "aTexCoord")
        GLES20.glEnableVertexAttribArray(aTexHandle)
        GLES20.glVertexAttribPointer(aTexHandle, 2, GLES20.GL_FLOAT, false, 0, texCoordBuffer)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(aPosHandle)
        GLES20.glDisableVertexAttribArray(aTexHandle)
    }

    private fun drainVideoEncoder(endOfStream: Boolean) {
        val codec = videoCodec ?: return
        val bufferInfo = MediaCodec.BufferInfo()
        var retryCount = 0

        while (true) {
            val encoderStatus = codec.dequeueOutputBuffer(bufferInfo, 10_000)
            if (encoderStatus == MediaCodec.INFO_TRY_AGAIN_LATER) {
                if (!endOfStream || ++retryCount > 10) break
            } else if (encoderStatus == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                synchronized(muxerLock) {
                    if (isMuxerStarted) {
                        Log.w(TAG, "Video format changed twice, ignoring")
                    } else {
                        val newFormat = codec.outputFormat
                        videoTrackIndex = mediaMuxer?.addTrack(newFormat) ?: -1
                        checkAndStartMuxer()
                    }
                }
            } else if (encoderStatus >= 0) {
                retryCount = 0
                val encodedData = codec.getOutputBuffer(encoderStatus)
                if (encodedData != null) {
                    if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                        bufferInfo.size = 0
                    }
                    if (bufferInfo.size > 0) {
                        synchronized(muxerLock) {
                            if (isMuxerStarted && videoTrackIndex >= 0) {
                                encodedData.position(bufferInfo.offset)
                                encodedData.limit(bufferInfo.offset + bufferInfo.size)
                                mediaMuxer?.writeSampleData(videoTrackIndex, encodedData, bufferInfo)
                            }
                        }
                    }
                }
                codec.releaseOutputBuffer(encoderStatus, false)
                if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    break
                }
            }
        }
    }

    private fun drainAudioEncoder(endOfStream: Boolean) {
        val codec = audioCodec ?: return
        val bufferInfo = MediaCodec.BufferInfo()
        var retryCount = 0

        while (true) {
            val encoderStatus = codec.dequeueOutputBuffer(bufferInfo, 10_000)
            if (encoderStatus == MediaCodec.INFO_TRY_AGAIN_LATER) {
                if (!endOfStream || ++retryCount > 10) break
            } else if (encoderStatus == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                synchronized(muxerLock) {
                    if (!isMuxerStarted) {
                        val newFormat = codec.outputFormat
                        audioTrackIndex = mediaMuxer?.addTrack(newFormat) ?: -1
                        checkAndStartMuxer()
                    }
                }
            } else if (encoderStatus >= 0) {
                retryCount = 0
                val encodedData = codec.getOutputBuffer(encoderStatus)
                if (encodedData != null) {
                    if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                        bufferInfo.size = 0
                    }
                    if (bufferInfo.size > 0) {
                        synchronized(muxerLock) {
                            if (isMuxerStarted && audioTrackIndex >= 0) {
                                encodedData.position(bufferInfo.offset)
                                encodedData.limit(bufferInfo.offset + bufferInfo.size)
                                mediaMuxer?.writeSampleData(audioTrackIndex, encodedData, bufferInfo)
                            }
                        }
                    }
                }
                codec.releaseOutputBuffer(encoderStatus, false)
                if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    break
                }
            }
        }
    }

    private fun checkAndStartMuxer() {
        val muxer = mediaMuxer ?: return
        val needsAudio = hasAudioPermission()
        val isReady = if (needsAudio) {
            videoTrackIndex >= 0 && audioTrackIndex >= 0
        } else {
            videoTrackIndex >= 0
        }

        if (isReady && !isMuxerStarted) {
            muxer.start()
            isMuxerStarted = true
            Log.i(TAG, "MediaMuxer started with videoTrack=$videoTrackIndex, audioTrack=$audioTrackIndex")
        }
    }

    private fun loadShader(type: Int, shaderCode: String): Int {
        return GLES20.glCreateShader(type).also { shader ->
            GLES20.glShaderSource(shader, shaderCode)
            GLES20.glCompileShader(shader)
        }
    }

    private fun releaseAll() {
        try {
            videoCodec?.stop()
            videoCodec?.release()
            videoCodec = null
        } catch (_: Exception) {}

        try {
            audioCodec?.stop()
            audioCodec?.release()
            audioCodec = null
        } catch (_: Exception) {}

        synchronized(muxerLock) {
            try {
                if (isMuxerStarted) {
                    mediaMuxer?.stop()
                }
                mediaMuxer?.release()
                mediaMuxer = null
                isMuxerStarted = false
            } catch (_: Exception) {}
        }

        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (eglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, eglSurface)
                eglSurface = EGL14.EGL_NO_SURFACE
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext)
                eglContext = EGL14.EGL_NO_CONTEXT
            }
            EGL14.eglTerminate(eglDisplay)
            eglDisplay = EGL14.EGL_NO_DISPLAY
        }

        inputSurface?.release()
        inputSurface = null
    }

    fun isRecordingNow(): Boolean = isRecording.get()
}
