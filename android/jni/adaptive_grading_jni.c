/**
 * @file adaptive_grading_jni.c
 * @brief Android JNI 桥接层核心实现
 */

#include "adaptive_grading_jni.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "android/bitmap.h"

#ifdef _WIN32
#include <windows.h>
static double get_time_ms_internal(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <time.h>
static double get_time_ms_internal(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

JNIEXPORT jlong JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeCreateEngine(
    JNIEnv* env,
    jclass clazz,
    jint defaultStyleId
) {
    (void)env; (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)calloc(1, sizeof(AdaptiveJniContext));
    if (!ctx) return 0;

    ctx->engine = adaptive_grading_create(defaultStyleId);
    ctx->logger = adaptive_logger_create();
    ctx->frame_counter = 0;

    if (!ctx->engine || !ctx->logger) {
        if (ctx->engine) adaptive_grading_destroy(ctx->engine);
        if (ctx->logger) adaptive_logger_destroy(ctx->logger);
        free(ctx);
        return 0;
    }

    return (jlong)(intptr_t)ctx;
}

JNIEXPORT void JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeDestroyEngine(
    JNIEnv* env,
    jclass clazz,
    jlong handle
) {
    (void)env; (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx) return;

    if (ctx->engine) adaptive_grading_destroy(ctx->engine);
    if (ctx->logger) adaptive_logger_destroy(ctx->logger);
    free(ctx);
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeSetStyle(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jint styleId
) {
    (void)env; (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx || !ctx->engine) return ADAPTIVE_ERR_NULL_POINTER;
    return (jint)adaptive_grading_set_style(ctx->engine, styleId);
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeGetStyle(
    JNIEnv* env,
    jclass clazz,
    jlong handle
) {
    (void)env; (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx || !ctx->engine) return -1;
    return (jint)adaptive_grading_get_style(ctx->engine);
}

JNIEXPORT void JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeResetSmoother(
    JNIEnv* env,
    jclass clazz,
    jlong handle
) {
    (void)env; (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (ctx && ctx->engine) {
        adaptive_grading_reset_smoother(ctx->engine);
    }
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeSetThreadCount(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jint threadCount
) {
    (void)env; (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx || !ctx->engine) return ADAPTIVE_ERR_NULL_POINTER;
    return (jint)adaptive_grading_set_thread_count(ctx->engine, threadCount);
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeGetThreadCount(
    JNIEnv* env,
    jclass clazz,
    jlong handle
) {
    (void)env; (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx || !ctx->engine) return 1;
    return (jint)adaptive_grading_get_thread_count(ctx->engine);
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeAnalyze(
    JNIEnv* env,
    jclass clazz,
    jobject srcBuffer,
    jint width,
    jint height,
    jint stride,
    jint format,
    jfloatArray outMetrics
) {
    (void)clazz;
    if (!srcBuffer) return ADAPTIVE_ERR_NULL_POINTER;
    const uint8_t* src_ptr = (const uint8_t*)(*env)->GetDirectBufferAddress(env, srcBuffer);
    if (!src_ptr) return ADAPTIVE_ERR_INVALID_PARAM;

    AdaptiveSceneMetrics metrics;
    AdaptiveResult res = adaptive_grading_analyze(
        src_ptr, width, height, stride, (AdaptivePixelFormat)format, &metrics
    );
    if (res != ADAPTIVE_OK) return (jint)res;

    if (outMetrics && (*env)->GetArrayLength(env, outMetrics) >= 8) {
        jfloat m_arr[8];
        m_arr[0] = metrics.mean_luminance;
        m_arr[1] = metrics.std_luminance;
        m_arr[2] = metrics.shadow_clip_ratio;
        m_arr[3] = metrics.highlight_clip_ratio;
        m_arr[4] = metrics.estimated_cct;
        m_arr[5] = metrics.tint;
        m_arr[6] = (jfloat)metrics.scene_type;
        m_arr[7] = metrics.dynamic_range_ev;
        (*env)->SetFloatArrayRegion(env, outMetrics, 0, 8, m_arr);
    }

    return ADAPTIVE_OK;
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeProcessFrameDirect(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jobject srcBuffer,
    jobject dstBuffer,
    jint width,
    jint height,
    jint srcStride,
    jint dstStride,
    jint format,
    jboolean splitMode,
    jfloat splitRatio,
    jfloatArray outMetrics,
    jfloatArray outParams
) {
    (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx || !ctx->engine || !srcBuffer || !dstBuffer) return ADAPTIVE_ERR_NULL_POINTER;

    const uint8_t* src_ptr = (const uint8_t*)(*env)->GetDirectBufferAddress(env, srcBuffer);
    uint8_t* dst_ptr = (uint8_t*)(*env)->GetDirectBufferAddress(env, dstBuffer);
    if (!src_ptr || !dst_ptr) return ADAPTIVE_ERR_INVALID_PARAM;

    double t0 = get_time_ms_internal();

    AdaptiveSceneMetrics metrics;
    AdaptiveStyleParameters active_params;

    AdaptiveResult res = adaptive_grading_process_frame(
        ctx->engine,
        src_ptr,
        dst_ptr,
        width,
        height,
        srcStride,
        dstStride,
        (AdaptivePixelFormat)format,
        (splitMode == JNI_TRUE) ? 1 : 0,
        (float)splitRatio,
        &metrics,
        &active_params
    );

    double t1 = get_time_ms_internal();
    float total_ms = (float)(t1 - t0);

    /* 记录 Native 高频环形日志 */
    if (ctx->logger) {
        ctx->frame_counter++;
        adaptive_logger_record_frame(
            ctx->logger,
            ctx->frame_counter,
            total_ms,
            1.0f, /* 感知时间 */
            total_ms - 1.0f, /* 渲染时间 */
            &metrics,
            &active_params,
            adaptive_grading_get_style(ctx->engine)
        );
    }

    /* 导出指标给 Java 层 (若上层需要渲染 HUD) */
    if (outMetrics && (*env)->GetArrayLength(env, outMetrics) >= 8) {
        jfloat m_arr[8];
        m_arr[0] = metrics.mean_luminance;
        m_arr[1] = metrics.std_luminance;
        m_arr[2] = metrics.shadow_clip_ratio;
        m_arr[3] = metrics.highlight_clip_ratio;
        m_arr[4] = metrics.estimated_cct;
        m_arr[5] = metrics.tint;
        m_arr[6] = (jfloat)metrics.scene_type;
        m_arr[7] = metrics.dynamic_range_ev;
        (*env)->SetFloatArrayRegion(env, outMetrics, 0, 8, m_arr);
    }

    /* 导出实际参数给 Java 层 */
    if (outParams && (*env)->GetArrayLength(env, outParams) >= 16) {
        jfloat p_arr[16];
        p_arr[0] = active_params.contrast;
        p_arr[1] = active_params.exposure_bias;
        p_arr[2] = active_params.saturation;
        p_arr[3] = active_params.black_lift;
        p_arr[4] = active_params.white_clip;
        p_arr[5] = active_params.toe_strength;
        p_arr[6] = active_params.shoulder_strength;
        p_arr[7] = active_params.grain_intensity;
        p_arr[8] = active_params.vignette;
        p_arr[9] = active_params.shadow_tint[0];
        p_arr[10] = active_params.shadow_tint[1];
        p_arr[11] = active_params.shadow_tint[2];
        p_arr[12] = (jfloat)active_params.protect_skin;
        p_arr[13] = (jfloat)active_params.is_monochrome;
        p_arr[14] = active_params.denoise_strength;
        p_arr[15] = active_params.clarity_strength;
        (*env)->SetFloatArrayRegion(env, outParams, 0, 16, p_arr);
    }

    return (jint)res;
}

extern int __android_log_print(int prio, const char* tag, const char* fmt, ...);
#define LOGE(...) __android_log_print(6, "GradingJNI", __VA_ARGS__)

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeProcessFrameYUV420(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jobject yBuffer,
    jobject uvBuffer,
    jobject dstRgbaBuffer,
    jint width,
    jint height,
    jint yStride,
    jint uvStride,
    jint dstStride,
    jboolean isNv21,
    jboolean splitMode,
    jfloat splitRatio,
    jfloatArray outMetrics,
    jfloatArray outParams
) {
    (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (ctx && ctx->frame_counter % 60 == 0) {
        LOGE("CameraGrading: Active frame %u (w=%d, h=%d, split=%d)", ctx->frame_counter, width, height, (int)splitMode);
    }

    if (!ctx || !ctx->engine || !yBuffer || !uvBuffer || !dstRgbaBuffer) {
        LOGE("ProcessYUV420: null pointer check failed: ctx=%p, eng=%p, yBuf=%p, uvBuf=%p, dstBuf=%p",
             ctx, ctx ? ctx->engine : NULL, yBuffer, uvBuffer, dstRgbaBuffer);
        return ADAPTIVE_ERR_NULL_POINTER;
    }

    const uint8_t* y_ptr = (const uint8_t*)(*env)->GetDirectBufferAddress(env, yBuffer);
    const uint8_t* uv_ptr = (const uint8_t*)(*env)->GetDirectBufferAddress(env, uvBuffer);
    uint8_t* dst_ptr = (uint8_t*)(*env)->GetDirectBufferAddress(env, dstRgbaBuffer);
    if (!y_ptr || !uv_ptr || !dst_ptr) {
        LOGE("ProcessYUV420: GetDirectBufferAddress failed: y_ptr=%p, uv_ptr=%p, dst_ptr=%p", y_ptr, uv_ptr, dst_ptr);
        return ADAPTIVE_ERR_INVALID_PARAM;
    }

    double t0 = get_time_ms_internal();

    AdaptiveSceneMetrics metrics;
    AdaptiveStyleParameters active_params;

    AdaptiveResult res = adaptive_grading_process_yuv420(
        ctx->engine,
        y_ptr,
        uv_ptr,
        dst_ptr,
        width,
        height,
        yStride,
        uvStride,
        dstStride,
        (isNv21 == JNI_TRUE) ? 1 : 0,
        (splitMode == JNI_TRUE) ? 1 : 0,
        (float)splitRatio,
        &metrics,
        &active_params
    );

    if (res != ADAPTIVE_OK) {
        LOGE("ProcessYUV420: adaptive_grading_process_yuv420 returned error %d", (int)res);
    }

    double t1 = get_time_ms_internal();
    float total_ms = (float)(t1 - t0);

    /* 记录 Native 高频环形日志 */
    if (ctx->logger) {
        ctx->frame_counter++;
        adaptive_logger_record_frame(
            ctx->logger,
            ctx->frame_counter,
            total_ms,
            0.5f,
            total_ms - 0.5f,
            &metrics,
            &active_params,
            adaptive_grading_get_style(ctx->engine)
        );
    }

    /* 导出指标给 Java 层 */
    if (outMetrics && (*env)->GetArrayLength(env, outMetrics) >= 8) {
        jfloat m_arr[8];
        m_arr[0] = metrics.mean_luminance;
        m_arr[1] = metrics.std_luminance;
        m_arr[2] = metrics.shadow_clip_ratio;
        m_arr[3] = metrics.highlight_clip_ratio;
        m_arr[4] = metrics.estimated_cct;
        m_arr[5] = metrics.tint;
        m_arr[6] = (jfloat)metrics.scene_type;
        m_arr[7] = metrics.dynamic_range_ev;
        (*env)->SetFloatArrayRegion(env, outMetrics, 0, 8, m_arr);
    }

    /* 导出实际参数给 Java 层 */
    if (outParams && (*env)->GetArrayLength(env, outParams) >= 16) {
        jfloat p_arr[16];
        p_arr[0] = active_params.contrast;
        p_arr[1] = active_params.exposure_bias;
        p_arr[2] = active_params.saturation;
        p_arr[3] = active_params.black_lift;
        p_arr[4] = active_params.white_clip;
        p_arr[5] = active_params.toe_strength;
        p_arr[6] = active_params.shoulder_strength;
        p_arr[7] = active_params.grain_intensity;
        p_arr[8] = active_params.vignette;
        p_arr[9] = active_params.shadow_tint[0];
        p_arr[10] = active_params.shadow_tint[1];
        p_arr[11] = active_params.shadow_tint[2];
        p_arr[12] = (jfloat)active_params.protect_skin;
        p_arr[13] = (jfloat)active_params.is_monochrome;
        p_arr[14] = active_params.denoise_strength;
        p_arr[15] = active_params.clarity_strength;
        (*env)->SetFloatArrayRegion(env, outParams, 0, 16, p_arr);
    }

    return (jint)res;
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeDumpLogs(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jstring dumpPath
) {
    (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx || !ctx->logger || !dumpPath) return ADAPTIVE_ERR_NULL_POINTER;

    const char* path_str = (*env)->GetStringUTFChars(env, dumpPath, NULL);
    if (!path_str) return ADAPTIVE_ERR_INVALID_PARAM;

    AdaptiveResult res = adaptive_logger_dump_json(ctx->logger, path_str);
    (*env)->ReleaseStringUTFChars(env, dumpPath, path_str);

    return (jint)res;
}

JNIEXPORT jstring JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeGetVersion(
    JNIEnv* env,
    jclass clazz
) {
    (void)clazz;
    return (*env)->NewStringUTF(env, adaptive_grading_get_version());
}

JNIEXPORT jint JNICALL
Java_com_antigravity_grading_AdaptiveGradingNative_nativeGradeBitmapInPlace(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jobject bitmap
) {
    (void)clazz;
    AdaptiveJniContext* ctx = (AdaptiveJniContext*)(intptr_t)handle;
    if (!ctx || !ctx->engine || !bitmap) return -1;

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return -2;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return -3;

    void* pixels = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) return -4;

    AdaptiveSceneMetrics metrics;
    AdaptiveStyleParameters active_params;

    AdaptiveResult res = adaptive_grading_process_frame(
        ctx->engine,
        (const uint8_t*)pixels,
        (uint8_t*)pixels,
        (int)info.width,
        (int)info.height,
        (int)info.stride,
        (int)info.stride,
        ADAPTIVE_PIXEL_FORMAT_RGBA8888,
        0,
        0.5f,
        &metrics,
        &active_params
    );

    AndroidBitmap_unlockPixels(env, bitmap);
    return (jint)res;
}

