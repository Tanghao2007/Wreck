/**
 * @file adaptive_grading.c
 * @brief 实时智能自适应调色引擎纯 C 核心库实现
 */

#include "adaptive_grading.h"
#include "adaptive_grading_neon.h"
#include "adaptive_thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================== */
/* 宏与内部工具函数                                                           */
/* ========================================================================== */

#define CLAMP(x, low, high) (((x) < (low)) ? (low) : (((x) > (high)) ? (high) : (x)))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

static inline uint8_t clamp_u8(float val) {
    if (val <= 0.0f) return 0;
    if (val >= 255.0f) return 255;
    return (uint8_t)(val + 0.5f);
}

static inline uint8_t clamp_i32_u8(int32_t val) {
    if (val <= 0) return 0;
    if (val >= 255) return 255;
    return (uint8_t)val;
}

/* ========================================================================== */
/* 引擎内部上下文结构体定义                                                   */
/* ========================================================================== */

struct AdaptiveGradingEngine {
    int current_style_id;
    AdaptiveSmootherConfig smoother_cfg;
    int smoother_initialized;
    AdaptiveStyleParameters smoothed_params;

    /* 预分配缓冲区 (避免每帧渲染时动态分配堆内存) */
    uint8_t thumb_bgr[ADAPTIVE_ANALYZER_TARGET_SIZE * ADAPTIVE_ANALYZER_TARGET_SIZE * 3];
    uint8_t thumb_gray[ADAPTIVE_ANALYZER_TARGET_SIZE * ADAPTIVE_ANALYZER_TARGET_SIZE];
    uint32_t histogram[256];

    /* 银盐颗粒池 (1024x1024 float) 与帧偏移索引 */
    float grain_pool[ADAPTIVE_GRAIN_POOL_SIZE * ADAPTIVE_GRAIN_POOL_SIZE];
    uint32_t grain_frame_idx;

    /* Master 查找表与黑白查找表 */
    uint8_t master_lut[256 * 3];
    uint8_t filmic_mono_lut[256];

    /* 动态工作缓冲区 (按最大支持分辨率如 1080p 预留或按需扩容) */
    int max_buf_width;
    int max_buf_height;
    uint8_t* temp_y;
    uint8_t* temp_cr;
    uint8_t* temp_cb;
    uint8_t* temp_blur;
    uint8_t* temp_clean_frame;
    uint8_t* temp_graded_frame;
    float* skin_mask_small;
    float* skin_mask_full;
    uint8_t* vignette_mask_u8;
    int cached_vignette_w;
    int cached_vignette_h;
    float cached_vignette_intensity;

    /* 多线程切片并发工作线程池 */
    AdaptiveThreadPool* thread_pool;
    int thread_count;
};

/* ========================================================================== */
/* 辅助元数据与版本函数                                                       */
/* ========================================================================== */

AdaptiveResult adaptive_grading_set_grain_pool(
    AdaptiveGradingEngine* engine,
    const float* pool_data
) {
    if (!engine || !pool_data) return ADAPTIVE_ERR_NULL_POINTER;
    memcpy(engine->grain_pool, pool_data, sizeof(engine->grain_pool));
    return ADAPTIVE_OK;
}

const char* adaptive_grading_get_version(void) {
    return "1.0.0-android-flagship-c";
}

const char* adaptive_style_id_to_name(int style_id) {
    switch (style_id) {
        case 1:  return "自然真实类";
        case 2:  return "清新日系类";
        case 3:  return "韩系柔光类";
        case 4:  return "胶片复古类";
        case 5:  return "港风复古类";
        case 6:  return "欧美复古/电影感类";
        case 7:  return "暗黑情绪类";
        case 8:  return "经典黑白类";
        case 9:  return "高饱和鲜艳类";
        case 10: return "低饱和莫兰迪/灰调类";
        case 11: return "赛博朋克/霓虹类";
        case 12: return "国风/古风类";
        case 13: return "法式浪漫类";
        case 14: return "商业时尚类";
        case 15: return "风光HDR/通透类";
        default: return "未知风格";
    }
}

const char* adaptive_style_id_to_english_name(int style_id) {
    switch (style_id) {
        case 1:  return "Natural Authentic";
        case 2:  return "Fresh Japanese Airy";
        case 3:  return "Korean Soft Glow";
        case 4:  return "Vintage Film";
        case 5:  return "Hong Kong Vintage 90s";
        case 6:  return "Western Cinematic";
        case 7:  return "Moody Dark / Noir";
        case 8:  return "Classic Monochromatic";
        case 9:  return "Vibrant Pop";
        case 10: return "Morandi Muted Gray";
        case 11: return "Cyberpunk Neon";
        case 12: return "Traditional Chinese Aesthetic";
        case 13: return "French Romantic";
        case 14: return "Commercial Editorial";
        case 15: return "Landscape HDR Clarity";
        default: return "Unknown Style";
    }
}

const char* adaptive_scene_type_to_string(AdaptiveSceneType type) {
    switch (type) {
        case ADAPTIVE_SCENE_LOW_LIGHT_NIGHT:  return "低照度夜景 (Low-Light Night)";
        case ADAPTIVE_SCENE_HIGH_DR_BACKLIT:  return "大光比逆光 (High-DR Backlit)";
        case ADAPTIVE_SCENE_GOLDEN_HOUR_WARM: return "暖阳/黄金时刻 (Golden Hour Warm)";
        case ADAPTIVE_SCENE_SOFT_OVERCAST:    return "高调平光/阴天 (Soft Overcast)";
        case ADAPTIVE_SCENE_SUNNY_DAYLIGHT:   return "晴朗日光 (Sunny Daylight)";
        case ADAPTIVE_SCENE_INDOOR_AMBIENT:   return "常规室内/漫反射 (Indoor Ambient)";
        default:                              return "未知场景 (Unknown)";
    }
}

/* ========================================================================== */
/* 内部色彩空间转换与下采样                                                   */
/* ========================================================================== */

/**
 * @brief 将输入帧下采样至 256x256 BGR 格式
 */
static void downsample_to_256x256(
    const uint8_t* src,
    int src_w,
    int src_h,
    int src_stride,
    AdaptivePixelFormat format,
    uint8_t* dst_bgr
) {
    int bpp = 3;
    int b_idx = 0, g_idx = 1, r_idx = 2;
    if (format == ADAPTIVE_PIXEL_FORMAT_RGB888) {
        b_idx = 2; g_idx = 1; r_idx = 0;
    } else if (format == ADAPTIVE_PIXEL_FORMAT_RGBA8888) {
        bpp = 4; b_idx = 2; g_idx = 1; r_idx = 0;
    } else if (format == ADAPTIVE_PIXEL_FORMAT_BGRA8888) {
        bpp = 4; b_idx = 0; g_idx = 1; r_idx = 2;
    }

    if (src_w == 256 && src_h == 256) {
        for (int y = 0; y < 256; y++) {
            const uint8_t* s_row = src + y * src_stride;
            uint8_t* d_row = dst_bgr + y * (256 * 3);
            for (int x = 0; x < 256; x++) {
                const uint8_t* px = s_row + x * bpp;
                d_row[x * 3 + 0] = px[b_idx];
                d_row[x * 3 + 1] = px[g_idx];
                d_row[x * 3 + 2] = px[r_idx];
            }
        }
        return;
    }

    /* 区域平均下采样 (Area Average Downsampling, 对应 OpenCV INTER_AREA) */
    float scale_x = (float)src_w / 256.0f;
    float scale_y = (float)src_h / 256.0f;

    for (int dy = 0; dy < 256; dy++) {
        int sy_start = (int)(dy * scale_y);
        int sy_end = (int)((dy + 1) * scale_y);
        if (sy_end <= sy_start) sy_end = sy_start + 1;
        if (sy_end > src_h) sy_end = src_h;

        uint8_t* d_row = dst_bgr + dy * (256 * 3);

        for (int dx = 0; dx < 256; dx++) {
            int sx_start = (int)(dx * scale_x);
            int sx_end = (int)((dx + 1) * scale_x);
            if (sx_end <= sx_start) sx_end = sx_start + 1;
            if (sx_end > src_w) sx_end = src_w;

            uint32_t sum_b = 0, sum_g = 0, sum_r = 0;
            uint32_t count = (sy_end - sy_start) * (sx_end - sx_start);

            for (int sy = sy_start; sy < sy_end; sy++) {
                const uint8_t* s_row = src + sy * src_stride;
                for (int sx = sx_start; sx < sx_end; sx++) {
                    const uint8_t* px = s_row + sx * bpp;
                    sum_b += px[b_idx];
                    sum_g += px[g_idx];
                    sum_r += px[r_idx];
                }
            }

            d_row[dx * 3 + 0] = (uint8_t)((sum_b + (count >> 1)) / count);
            d_row[dx * 3 + 1] = (uint8_t)((sum_g + (count >> 1)) / count);
            d_row[dx * 3 + 2] = (uint8_t)((sum_r + (count >> 1)) / count);
        }
    }
}

/**
 * @brief BGR 转 Grayscale (标准 Rec.601: Y = 0.114*B + 0.587*G + 0.299*R)
 */
static void bgr256_to_gray(const uint8_t* bgr, uint8_t* gray) {
    for (int i = 0; i < 256 * 256; i++) {
        int b = bgr[i * 3 + 0];
        int g = bgr[i * 3 + 1];
        int r = bgr[i * 3 + 2];
        /* 与 OpenCV cvtColor(BGR2GRAY) 保持整型舍入完全一致 */
        int y = (b * 114 + g * 587 + r * 299 + 500) / 1000;
        gray[i] = (uint8_t)CLAMP(y, 0, 255);
    }
}

/* ========================================================================== */
/* 场景感知分析器 (SceneAnalyzer) 纯 C 实现                                   */
/* ========================================================================== */

static AdaptiveSceneType classify_scene_heuristic(
    float mean_lum,
    float std_lum,
    float dr_ev,
    float rb_ratio,
    float highlight_clip
) {
    if (mean_lum < 0.22f) {
        return ADAPTIVE_SCENE_LOW_LIGHT_NIGHT;
    } else if (dr_ev > 9.5f || (std_lum > 0.28f && highlight_clip > 0.05f)) {
        return ADAPTIVE_SCENE_HIGH_DR_BACKLIT;
    } else if (rb_ratio > 1.35f && mean_lum > 0.35f) {
        return ADAPTIVE_SCENE_GOLDEN_HOUR_WARM;
    } else if (mean_lum > 0.65f && std_lum < 0.18f) {
        return ADAPTIVE_SCENE_SOFT_OVERCAST;
    } else if (mean_lum >= 0.45f && std_lum >= 0.20f) {
        return ADAPTIVE_SCENE_SUNNY_DAYLIGHT;
    } else {
        return ADAPTIVE_SCENE_INDOOR_AMBIENT;
    }
}

AdaptiveResult adaptive_grading_analyze(
    const uint8_t* src_data,
    int width,
    int height,
    int stride,
    AdaptivePixelFormat format,
    AdaptiveSceneMetrics* out_metrics
) {
    if (!src_data || !out_metrics || width <= 0 || height <= 0) {
        return ADAPTIVE_ERR_INVALID_PARAM;
    }

    uint8_t thumb_bgr[256 * 256 * 3];
    uint8_t thumb_gray[256 * 256];
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));

    /* 1. 下采样 */
    downsample_to_256x256(src_data, width, height, stride, format, thumb_bgr);
    bgr256_to_gray(thumb_bgr, thumb_gray);

    const int total_pixels = 256 * 256;
    double sum_lum = 0.0;
    double sum_lum_sq = 0.0;
    int shadow_clip_cnt = 0;
    int highlight_clip_cnt = 0;

    double sum_b = 0.0, sum_g = 0.0, sum_r = 0.0;
    double bright_sum_b = 0.0, bright_sum_g = 0.0, bright_sum_r = 0.0;
    int bright_cnt = 0;

    /* 2. 单通道循环累加 (与 Python 统计学保持绝对一致) */
    for (int i = 0; i < total_pixels; i++) {
        uint8_t y = thumb_gray[i];
        hist[y]++;

        double yf = (double)y / 255.0;
        sum_lum += yf;
        sum_lum_sq += yf * yf;

        if (y < 12) shadow_clip_cnt++;
        if (y > 245) highlight_clip_cnt++;

        uint8_t b = thumb_bgr[i * 3 + 0];
        uint8_t g = thumb_bgr[i * 3 + 1];
        uint8_t r = thumb_bgr[i * 3 + 2];
        sum_b += b;
        sum_g += g;
        sum_r += r;

        if (y >= 170 && y <= 245) {
            bright_sum_b += b;
            bright_sum_g += g;
            bright_sum_r += r;
            bright_cnt++;
        }
    }

    float mean_lum = (float)(sum_lum / total_pixels);
    double variance = (sum_lum_sq / total_pixels) - ((double)mean_lum * (double)mean_lum);
    float std_lum = (float)sqrt(MAX(0.0, variance));

    float shadow_clip = (float)shadow_clip_cnt / total_pixels;
    float highlight_clip = (float)highlight_clip_cnt / total_pixels;

    /* 3. 分位数估算动态范围 (2% 与 98%) */
    int target_02 = (int)(total_pixels * 0.02);
    int target_98 = (int)(total_pixels * 0.98);
    int cum = 0;
    int p02_bin = 0, p98_bin = 255;
    int found_02 = 0;

    for (int bin = 0; bin < 256; bin++) {
        cum += hist[bin];
        if (!found_02 && cum >= target_02) {
            p02_bin = bin;
            found_02 = 1;
        }
        if (cum >= target_98) {
            p98_bin = bin;
            break;
        }
    }

    float p02 = (float)p02_bin / 255.0f;
    float p98 = (float)p98_bin / 255.0f;
    float ratio = MAX(p98, 0.01f) / MAX(p02, 0.002f);
    float dr_ev = (float)(log(ratio) / log(2.0));
    dr_ev = CLAMP(dr_ev, 3.0f, 14.0f);

    /* 4. 白平衡与色温估计 */
    double b_mean = sum_b / total_pixels;
    double g_mean = sum_g / total_pixels;
    double r_mean = sum_r / total_pixels;

    double b_ref, g_ref, r_ref;
    if (bright_cnt > 100) {
        double b_sample = bright_sum_b / bright_cnt;
        double g_sample = bright_sum_g / bright_cnt;
        double r_sample = bright_sum_r / bright_cnt;
        b_ref = 0.6 * b_sample + 0.4 * b_mean;
        g_ref = 0.6 * g_sample + 0.4 * g_mean;
        r_ref = 0.6 * r_sample + 0.4 * r_mean;
    } else {
        b_ref = b_mean;
        g_ref = g_mean;
        r_ref = r_mean;
    }

    float rb_ratio = (float)((r_ref + 1e-4) / (b_ref + 1e-4));
    float estimated_cct = 5500.0f / (float)pow(rb_ratio, 0.85);
    estimated_cct = CLAMP(estimated_cct, 2400.0f, 9500.0f);

    float expected_g = (float)((r_ref + b_ref) * 0.5);
    float tint = (float)((g_ref - expected_g) / MAX(expected_g, 1.0f));
    tint = CLAMP(tint, -1.0f, 1.0f);

    /* 5. 场景分类 */
    AdaptiveSceneType scene_type = classify_scene_heuristic(
        mean_lum, std_lum, dr_ev, rb_ratio, highlight_clip
    );

    out_metrics->mean_luminance = mean_lum;
    out_metrics->std_luminance = std_lum;
    out_metrics->shadow_clip_ratio = shadow_clip;
    out_metrics->highlight_clip_ratio = highlight_clip;
    out_metrics->estimated_cct = estimated_cct;
    out_metrics->tint = tint;
    out_metrics->scene_type = scene_type;
    out_metrics->dynamic_range_ev = dr_ev;
    snprintf(out_metrics->scene_label, sizeof(out_metrics->scene_label), "%s", adaptive_scene_type_to_string(scene_type));

    return ADAPTIVE_OK;
}

/* ========================================================================== */
/* 15 种审美风格解算器 (Style Solvers) 纯 C 实现                              */
/* ========================================================================== */

AdaptiveResult adaptive_grading_solve_style(
    int style_id,
    const AdaptiveSceneMetrics* m,
    AdaptiveStyleParameters* p
) {
    if (!m || !p) return ADAPTIVE_ERR_NULL_POINTER;
    if (style_id < 1 || style_id > 15) return ADAPTIVE_ERR_INVALID_PARAM;

    memset(p, 0, sizeof(AdaptiveStyleParameters));

    switch (style_id) {
        case 1: { /* 01. 自然真实类 */
            float cct_err = (m->estimated_cct - 5500.0f) / 4000.0f;
            float b_comp = CLAMP(-cct_err * 0.04f, -0.05f, 0.05f);
            float r_comp = CLAMP(cct_err * 0.04f, -0.05f, 0.05f);
            float lift = (m->dynamic_range_ev > 8.5f) ? 0.015f : 0.005f;
            float clip = (m->highlight_clip_ratio > 0.02f) ? 0.98f : 1.0f;

            p->contrast = 1.04f;
            p->exposure_bias = 0.0f;
            p->saturation = 1.0f;
            p->black_lift = lift;
            p->white_clip = clip;
            p->toe_strength = 0.45f;
            p->shoulder_strength = 0.50f;
            p->grain_intensity = 0.0f;
            p->vignette = 0.0f;
            p->shadow_tint[0] = b_comp; p->shadow_tint[1] = 0.0f; p->shadow_tint[2] = r_comp;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.35f;
            p->clarity_strength = 0.35f;
            break;
        }
        case 2: { /* 02. 清新日系类 */
            float lift = 0.06f + m->shadow_clip_ratio * 0.08f;
            lift = CLAMP(lift, 0.05f, 0.12f);
            float ev = (m->mean_luminance < 0.65f) ? 0.30f : 0.15f;

            p->contrast = 0.88f;
            p->exposure_bias = ev;
            p->saturation = 0.82f;
            p->black_lift = lift;
            p->white_clip = 0.94f;
            p->toe_strength = 0.25f;
            p->shoulder_strength = 0.85f;
            p->grain_intensity = 0.005f;
            p->vignette = 0.02f;
            p->shadow_tint[0] = 0.04f; p->shadow_tint[1] = 0.03f; p->shadow_tint[2] = -0.02f;
            p->mid_tint[0] = -0.01f;   p->mid_tint[1] = 0.02f;   p->mid_tint[2] = 0.0f;
            p->highlight_tint[0] = 0.01f; p->highlight_tint[1] = 0.03f; p->highlight_tint[2] = 0.02f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.40f;
            p->clarity_strength = 0.25f;
            break;
        }
        case 3: { /* 03. 韩系柔光类 */
            float ev = (m->mean_luminance < 0.60f) ? 0.22f : 0.08f;
            p->contrast = 0.86f;
            p->exposure_bias = ev;
            p->saturation = 0.85f;
            p->black_lift = 0.05f;
            p->white_clip = 0.92f;
            p->toe_strength = 0.30f;
            p->shoulder_strength = 0.90f;
            p->grain_intensity = 0.0f;
            p->vignette = 0.05f;
            p->shadow_tint[0] = -0.01f; p->shadow_tint[1] = 0.01f; p->shadow_tint[2] = 0.02f;
            p->mid_tint[0] = -0.03f;    p->mid_tint[1] = 0.01f;    p->mid_tint[2] = 0.04f;
            p->highlight_tint[0] = -0.02f; p->highlight_tint[1] = 0.02f; p->highlight_tint[2] = 0.04f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.45f;
            p->clarity_strength = 0.20f;
            break;
        }
        case 4: { /* 04. 胶片复古类 */
            p->contrast = 1.14f;
            p->exposure_bias = 0.0f;
            p->saturation = 0.88f;
            p->black_lift = 0.06f;
            p->white_clip = 0.96f;
            p->toe_strength = 0.60f;
            p->shoulder_strength = 0.70f;
            p->grain_intensity = 0.032f;
            p->vignette = 0.22f;
            p->shadow_tint[0] = 0.05f; p->shadow_tint[1] = 0.04f; p->shadow_tint[2] = -0.04f;
            p->mid_tint[0] = 0.0f;     p->mid_tint[1] = 0.01f;    p->mid_tint[2] = 0.02f;
            p->highlight_tint[0] = -0.05f; p->highlight_tint[1] = 0.01f; p->highlight_tint[2] = 0.05f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.20f;
            p->clarity_strength = 0.30f;
            break;
        }
        case 5: { /* 05. 港风复古类 */
            float c = 1.34f + (0.22f - m->std_luminance) * 0.5f;
            p->contrast = CLAMP(c, 1.25f, 1.48f);
            p->exposure_bias = -0.04f;
            p->saturation = 1.18f;
            p->black_lift = 0.015f;
            p->white_clip = 0.98f;
            p->toe_strength = 0.80f;
            p->shoulder_strength = 0.55f;
            p->grain_intensity = 0.036f;
            p->vignette = 0.28f;
            p->shadow_tint[0] = 0.04f; p->shadow_tint[1] = 0.05f; p->shadow_tint[2] = -0.04f;
            p->mid_tint[0] = -0.02f;   p->mid_tint[1] = 0.02f;    p->mid_tint[2] = 0.05f;
            p->highlight_tint[0] = -0.06f; p->highlight_tint[1] = 0.01f; p->highlight_tint[2] = 0.07f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.25f;
            p->clarity_strength = 0.35f;
            break;
        }
        case 6: { /* 06. 欧美复古/电影感类 */
            p->contrast = 1.30f;
            p->exposure_bias = -0.03f;
            p->saturation = 0.95f;
            p->black_lift = 0.018f;
            p->white_clip = 0.96f;
            p->toe_strength = 0.72f;
            p->shoulder_strength = 0.68f;
            p->grain_intensity = 0.024f;
            p->vignette = 0.26f;
            p->shadow_tint[0] = 0.09f; p->shadow_tint[1] = 0.03f; p->shadow_tint[2] = -0.07f;
            p->mid_tint[0] = 0.0f;     p->mid_tint[1] = 0.01f;    p->mid_tint[2] = 0.03f;
            p->highlight_tint[0] = -0.08f; p->highlight_tint[1] = 0.01f; p->highlight_tint[2] = 0.07f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.30f;
            p->clarity_strength = 0.40f;
            break;
        }
        case 7: { /* 07. 暗黑情绪类 */
            float ev = (m->mean_luminance > 0.30f) ? -0.35f : -0.15f;
            p->contrast = 1.24f;
            p->exposure_bias = ev;
            p->saturation = 0.68f;
            p->black_lift = 0.01f;
            p->white_clip = 0.92f;
            p->toe_strength = 0.85f;
            p->shoulder_strength = 0.45f;
            p->grain_intensity = 0.035f;
            p->vignette = 0.40f;
            p->shadow_tint[0] = 0.08f; p->shadow_tint[1] = 0.02f;  p->shadow_tint[2] = -0.05f;
            p->mid_tint[0] = 0.03f;    p->mid_tint[1] = -0.01f; p->mid_tint[2] = -0.02f;
            p->highlight_tint[0] = -0.02f; p->highlight_tint[1] = 0.0f; p->highlight_tint[2] = 0.02f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.35f;
            p->clarity_strength = 0.30f;
            break;
        }
        case 8: { /* 08. 经典黑白类 */
            float c = 1.45f + (0.22f - m->std_luminance) * 0.6f;
            p->contrast = CLAMP(c, 1.35f, 1.75f);
            p->exposure_bias = 0.0f;
            p->saturation = 0.0f;
            p->black_lift = 0.0f;
            p->white_clip = 1.0f;
            p->toe_strength = 0.85f;
            p->shoulder_strength = 0.55f;
            p->grain_intensity = 0.030f;
            p->vignette = 0.22f;
            p->protect_skin = 0;
            p->is_monochrome = 1;
            p->denoise_strength = 0.25f;
            p->clarity_strength = 0.45f;
            break;
        }
        case 9: { /* 09. 高饱和鲜艳类 */
            float sat = (m->std_luminance < 0.25f) ? 1.35f : 1.25f;
            p->contrast = 1.20f;
            p->exposure_bias = 0.05f;
            p->saturation = sat;
            p->black_lift = 0.0f;
            p->white_clip = 0.98f;
            p->toe_strength = 0.60f;
            p->shoulder_strength = 0.60f;
            p->grain_intensity = 0.0f;
            p->vignette = 0.06f;
            p->mid_tint[1] = 0.01f; p->mid_tint[2] = 0.02f;
            p->highlight_tint[0] = -0.02f; p->highlight_tint[1] = 0.01f; p->highlight_tint[2] = 0.02f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.35f;
            p->clarity_strength = 0.48f;
            break;
        }
        case 10: { /* 10. 低饱和莫兰迪/灰调类 */
            p->contrast = 0.92f;
            p->exposure_bias = 0.0f;
            p->saturation = 0.66f;
            p->black_lift = 0.05f;
            p->white_clip = 0.94f;
            p->toe_strength = 0.35f;
            p->shoulder_strength = 0.80f;
            p->grain_intensity = 0.0f;
            p->vignette = 0.08f;
            p->shadow_tint[0] = 0.02f; p->shadow_tint[1] = 0.01f; p->shadow_tint[2] = 0.01f;
            p->mid_tint[0] = -0.01f;   p->mid_tint[1] = 0.01f;    p->mid_tint[2] = 0.02f;
            p->highlight_tint[0] = -0.02f; p->highlight_tint[1] = 0.01f; p->highlight_tint[2] = 0.03f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.40f;
            p->clarity_strength = 0.25f;
            break;
        }
        case 11: { /* 11. 赛博朋克/霓虹类 */
            p->contrast = 1.38f;
            p->exposure_bias = -0.12f;
            p->saturation = 1.26f;
            p->black_lift = 0.02f;
            p->white_clip = 0.98f;
            p->toe_strength = 0.75f;
            p->shoulder_strength = 0.50f;
            p->grain_intensity = 0.018f;
            p->vignette = 0.32f;
            p->shadow_tint[0] = 0.12f; p->shadow_tint[1] = -0.02f; p->shadow_tint[2] = 0.04f;
            p->mid_tint[0] = 0.04f;    p->mid_tint[1] = -0.01f;  p->mid_tint[2] = 0.04f;
            p->highlight_tint[0] = -0.06f; p->highlight_tint[1] = 0.03f; p->highlight_tint[2] = 0.08f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.25f;
            p->clarity_strength = 0.45f;
            break;
        }
        case 12: { /* 12. 国风/古风类 */
            p->contrast = 1.06f;
            p->exposure_bias = 0.04f;
            p->saturation = 0.78f;
            p->black_lift = 0.04f;
            p->white_clip = 0.95f;
            p->toe_strength = 0.40f;
            p->shoulder_strength = 0.75f;
            p->grain_intensity = 0.012f;
            p->vignette = 0.16f;
            p->shadow_tint[0] = 0.03f; p->shadow_tint[1] = 0.04f; p->shadow_tint[2] = -0.02f;
            p->mid_tint[0] = -0.01f;   p->mid_tint[1] = 0.02f;    p->mid_tint[2] = 0.02f;
            p->highlight_tint[0] = -0.03f; p->highlight_tint[1] = 0.02f; p->highlight_tint[2] = 0.04f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.35f;
            p->clarity_strength = 0.30f;
            break;
        }
        case 13: { /* 13. 法式浪漫类 */
            float ev = (m->mean_luminance < 0.65f) ? 0.18f : 0.06f;
            p->contrast = 0.93f;
            p->exposure_bias = ev;
            p->saturation = 0.88f;
            p->black_lift = 0.05f;
            p->white_clip = 0.93f;
            p->toe_strength = 0.32f;
            p->shoulder_strength = 0.82f;
            p->grain_intensity = 0.016f;
            p->vignette = 0.14f;
            p->shadow_tint[0] = -0.02f; p->shadow_tint[1] = 0.01f; p->shadow_tint[2] = 0.03f;
            p->mid_tint[0] = -0.03f;    p->mid_tint[1] = 0.01f;    p->mid_tint[2] = 0.04f;
            p->highlight_tint[0] = -0.04f; p->highlight_tint[1] = 0.02f; p->highlight_tint[2] = 0.05f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.35f;
            p->clarity_strength = 0.25f;
            break;
        }
        case 14: { /* 14. 商业时尚类 */
            p->contrast = 1.24f;
            p->exposure_bias = 0.0f;
            p->saturation = 1.04f;
            p->black_lift = 0.01f;
            p->white_clip = 0.98f;
            p->toe_strength = 0.55f;
            p->shoulder_strength = 0.60f;
            p->grain_intensity = 0.0f;
            p->vignette = 0.10f;
            p->shadow_tint[0] = 0.01f; p->shadow_tint[1] = 0.01f; p->shadow_tint[2] = -0.01f;
            p->highlight_tint[2] = 0.01f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.40f;
            p->clarity_strength = 0.55f;
            break;
        }
        case 15: { /* 15. 风光HDR/通透类 */
            float toe = (m->dynamic_range_ev > 8.0f) ? 0.32f : 0.45f;
            float shoulder = (m->highlight_clip_ratio > 0.02f) ? 0.88f : 0.70f;
            p->contrast = 1.14f;
            p->exposure_bias = 0.04f;
            p->saturation = 1.20f;
            p->black_lift = 0.02f;
            p->white_clip = 0.96f;
            p->toe_strength = toe;
            p->shoulder_strength = shoulder;
            p->grain_intensity = 0.0f;
            p->vignette = 0.06f;
            p->shadow_tint[0] = 0.02f; p->shadow_tint[1] = 0.02f; p->shadow_tint[2] = 0.0f;
            p->mid_tint[1] = 0.01f;    p->mid_tint[2] = 0.01f;
            p->highlight_tint[0] = -0.02f; p->highlight_tint[1] = 0.01f; p->highlight_tint[2] = 0.02f;
            p->protect_skin = 1;
            p->is_monochrome = 0;
            p->denoise_strength = 0.30f;
            p->clarity_strength = 0.60f;
            break;
        }
    }

    return ADAPTIVE_OK;
}

/* ========================================================================== */
/* 时域平滑器 (TemporalSmoother) 纯 C 实现                                    */
/* ========================================================================== */

static float smooth_single_param(
    float current_val,
    float target_val,
    const AdaptiveSmootherConfig* cfg
) {
    float diff = fabsf(target_val - current_val);
    float scale = MAX(fabsf(current_val), 1.0f);
    float rel_diff = diff / scale;

    float alpha;
    if (rel_diff > cfg->threshold) {
        float ratio = MIN((rel_diff - cfg->threshold) / cfg->threshold, 1.0f);
        alpha = cfg->base_alpha + ratio * (cfg->fast_alpha - cfg->base_alpha);
    } else {
        alpha = cfg->base_alpha;
    }

    return current_val + alpha * (target_val - current_val);
}

static void smoother_update_all(
    AdaptiveGradingEngine* engine,
    const AdaptiveStyleParameters* target,
    AdaptiveStyleParameters* out_smoothed
) {
    if (!engine->smoother_initialized) {
        *out_smoothed = *target;
        engine->smoothed_params = *target;
        engine->smoother_initialized = 1;
        return;
    }

    const AdaptiveSmootherConfig* cfg = &engine->smoother_cfg;
    AdaptiveStyleParameters* cur = &engine->smoothed_params;

    cur->contrast = smooth_single_param(cur->contrast, target->contrast, cfg);
    cur->exposure_bias = smooth_single_param(cur->exposure_bias, target->exposure_bias, cfg);
    cur->saturation = smooth_single_param(cur->saturation, target->saturation, cfg);
    cur->black_lift = smooth_single_param(cur->black_lift, target->black_lift, cfg);
    cur->white_clip = smooth_single_param(cur->white_clip, target->white_clip, cfg);
    cur->toe_strength = smooth_single_param(cur->toe_strength, target->toe_strength, cfg);
    cur->shoulder_strength = smooth_single_param(cur->shoulder_strength, target->shoulder_strength, cfg);
    cur->grain_intensity = smooth_single_param(cur->grain_intensity, target->grain_intensity, cfg);
    cur->vignette = smooth_single_param(cur->vignette, target->vignette, cfg);

    for (int i = 0; i < 3; i++) {
        cur->shadow_tint[i] = smooth_single_param(cur->shadow_tint[i], target->shadow_tint[i], cfg);
        cur->mid_tint[i] = smooth_single_param(cur->mid_tint[i], target->mid_tint[i], cfg);
        cur->highlight_tint[i] = smooth_single_param(cur->highlight_tint[i], target->highlight_tint[i], cfg);
    }

    cur->denoise_strength = smooth_single_param(cur->denoise_strength, target->denoise_strength, cfg);
    cur->clarity_strength = smooth_single_param(cur->clarity_strength, target->clarity_strength, cfg);

    cur->protect_skin = target->protect_skin;
    cur->is_monochrome = target->is_monochrome;

    *out_smoothed = *cur;
}

/* ========================================================================== */
/* 高精度图像处理算子 (降噪、清晰度增强、Master LUT、颗粒与暗角)              */
/* ========================================================================== */

/**
 * @brief 分离式高斯模糊 (Separable 1D Gaussian Blur with Replicate Border)
 */
static void gaussian_blur_u8(
    const uint8_t* src,
    uint8_t* dst,
    float* temp,
    int width,
    int height,
    const float* kernel,
    int ksize
) {
    int radius = ksize / 2;

    /* 水平扫描滤波 (保持 float 高精度累加，与 OpenCV sepFilter2D 100% 吻合) */
    for (int y = 0; y < height; y++) {
        const uint8_t* s_row = src + y * width;
        float* t_row = temp + y * width;
        for (int x = 0; x < width; x++) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; k++) {
                int sx = x + k;
                if (sx < 0) sx = 0;
                else if (sx >= width) sx = width - 1;
                sum += (float)s_row[sx] * kernel[k + radius];
            }
            t_row[x] = sum;
        }
    }

    /* 垂直扫描滤波 */
    for (int y = 0; y < height; y++) {
        uint8_t* d_row = dst + y * width;
        for (int x = 0; x < width; x++) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; k++) {
                int sy = y + k;
                if (sy < 0) sy = 0;
                else if (sy >= height) sy = height - 1;
                sum += temp[sy * width + x] * kernel[k + radius];
            }
            d_row[x] = clamp_u8(sum);
        }
    }
}

/**
 * @brief 骁龙 8 至尊版画质优化：色度降噪与微反差清晰度增强
 */
static void enhance_clarity_and_denoise_c(
    AdaptiveGradingEngine* engine,
    const uint8_t* src_bgr,
    uint8_t* dst_bgr,
    int width,
    int height,
    int bpp,
    float denoise_strength,
    float clarity_strength
) {
    if (denoise_strength <= 0.01f && clarity_strength <= 0.01f) {
        if (src_bgr != dst_bgr) {
            memcpy(dst_bgr, src_bgr, width * height * bpp);
        }
        return;
    }

    int total = width * height;
    uint8_t* y_plane = engine->temp_y;
    uint8_t* cr_plane = engine->temp_cr;
    uint8_t* cb_plane = engine->temp_cb;
    uint8_t* blur_plane = engine->temp_blur;
    float* blur_temp = (float*)engine->temp_graded_frame;

    /* 1. BGR -> YCrCb (分离明度与色度，与 OpenCV 保持绝对一致) */
#if defined(__ARM_NEON) || defined(__aarch64__)
    neon_bgr_to_ycrcb(src_bgr, y_plane, cr_plane, cb_plane, total, bpp);
#else
    for (int i = 0; i < total; i++) {
        int b = src_bgr[i * bpp + 0];
        int g = src_bgr[i * bpp + 1];
        int r = src_bgr[i * bpp + 2];

        int y = (int)(0.114f * b + 0.587f * g + 0.299f * r + 0.5f);
        int cr = (int)((r - y) * 0.713f + 128.0f + 0.5f);
        int cb = (int)((b - y) * 0.564f + 128.0f + 0.5f);

        y_plane[i] = (uint8_t)CLAMP(y, 0, 255);
        cr_plane[i] = (uint8_t)CLAMP(cr, 0, 255);
        cb_plane[i] = (uint8_t)CLAMP(cb, 0, 255);
    }
#endif

    /* 2. 色度去噪 (Chroma Denoise: 5x5 Gaussian, sigma=1.2) */
    if (denoise_strength > 0.05f) {
        static const float k5[5] = {0.08562916f, 0.2426676f, 0.34340648f, 0.2426676f, 0.08562916f};
        float alpha_c = MIN(1.0f, denoise_strength * 1.5f);

        /* 滤波 Cr 通道 */
        gaussian_blur_u8(cr_plane, blur_plane, blur_temp, width, height, k5, 5);
        for (int i = 0; i < total; i++) {
            float v = cr_plane[i] * (1.0f - alpha_c) + blur_plane[i] * alpha_c;
            cr_plane[i] = clamp_u8(v);
        }

        /* 滤波 Cb 通道 */
        gaussian_blur_u8(cb_plane, blur_plane, blur_temp, width, height, k5, 5);
        for (int i = 0; i < total; i++) {
            float v = cb_plane[i] * (1.0f - alpha_c) + blur_plane[i] * alpha_c;
            cb_plane[i] = clamp_u8(v);
        }
    }

    /* 3. 亮度通道自适应清晰度 (Unsharp Mask with Coring: 9x9 Gaussian, sigma=1.5) */
    if (clarity_strength > 0.05f) {
        static const float k9[9] = {
            0.00761442f, 0.03607497f, 0.10958608f, 0.21344454f, 0.26655997f,
            0.21344454f, 0.10958608f, 0.03607497f, 0.00761442f
        };
        gaussian_blur_u8(y_plane, blur_plane, blur_temp, width, height, k9, 9);

        const int noise_coring_thresh = 4;
        float gain = 1.0f + clarity_strength;
        float denoise_gain = 1.0f - denoise_strength * 0.5f;

        for (int i = 0; i < total; i++) {
            int y_orig = y_plane[i];
            int y_b = blur_plane[i];
            int diff = y_orig - y_b;
            float enhanced_diff;

            if (abs(diff) < noise_coring_thresh) {
                enhanced_diff = diff * denoise_gain;
            } else {
                enhanced_diff = diff * gain;
            }

            int y_sharp = (int)(y_b + enhanced_diff);
            y_plane[i] = (uint8_t)CLAMP(y_sharp, 0, 255);
        }
    }

    /* 4. YCrCb -> BGR 恢复 (与 OpenCV COLOR_YCrCb2BGR 保持完全一致) */
#if defined(__ARM_NEON) || defined(__aarch64__)
    neon_ycrcb_to_bgr(y_plane, cr_plane, cb_plane, dst_bgr, total, bpp);
#else
    for (int i = 0; i < total; i++) {
        int y = y_plane[i];
        int cr = cr_plane[i] - 128;
        int cb = cb_plane[i] - 128;

        int r = (int)(y + 1.403f * cr + 0.5f);
        int g = (int)(y - 0.714f * cr - 0.344f * cb + 0.5f);
        int b = (int)(y + 1.773f * cb + 0.5f);

        dst_bgr[i * bpp + 0] = (uint8_t)CLAMP(b, 0, 255);
        dst_bgr[i * bpp + 1] = (uint8_t)CLAMP(g, 0, 255);
        dst_bgr[i * bpp + 2] = (uint8_t)CLAMP(r, 0, 255);
        if (bpp == 4) {
            dst_bgr[i * bpp + 3] = src_bgr[i * bpp + 3];
        }
    }
#endif
}

/**
 * @brief 构建 Filmic S-Curve 1D LUT (单色黑白专用)
 */
static void build_filmic_curve_lut_c(
    float contrast,
    float toe_strength,
    float shoulder_strength,
    float black_lift,
    float white_clip,
    uint8_t* out_lut
) {
    float k = MAX(0.1f, contrast);
    float s_min = 1.0f / (1.0f + expf(k * 4.0f));
    float s_max = 1.0f / (1.0f + expf(-k * 4.0f));
    float mix = CLAMP(0.2f + 0.8f * (contrast / 2.0f), 0.0f, 1.0f);

    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        float s_curve = 1.0f / (1.0f + expf(-k * 8.0f * (x - 0.5f)));
        float s_norm = (s_curve - s_min) / (s_max - s_min + 1e-6f);
        float blended = (1.0f - mix) * x + mix * s_norm;

        if (black_lift > 0.0f) {
            float lift_mask = (1.0f - x) * (1.0f - x);
            blended += black_lift * lift_mask;
        }

        if (shoulder_strength > 0.0f) {
            float h_mask = MAX(0.0f, x - 0.6f) / 0.4f;
            blended *= (1.0f - h_mask * 0.15f * shoulder_strength);
        }

        float out = CLAMP(blended, 0.0f, white_clip) * (255.0f / MAX(white_clip, 1e-4f));
        out_lut[i] = clamp_u8(out);
    }
}

/**
 * @brief 构建 Master 3-Channel LUT (曝光 + Filmic S曲线 + 暗部抬升 + 三路色轮色彩平衡)
 */
static void build_master_grading_lut_c(
    const AdaptiveStyleParameters* p,
    uint8_t* out_lut3
) {
    float k = MAX(0.1f, p->contrast);
    float s_min = 1.0f / (1.0f + expf(k * 4.0f));
    float s_max = 1.0f / (1.0f + expf(-k * 4.0f));
    float mix = CLAMP(0.2f + 0.8f * (p->contrast / 2.0f), 0.0f, 1.0f);
    float exp_scale = (fabsf(p->exposure_bias) > 0.01f) ? powf(2.0f, p->exposure_bias) : 1.0f;

    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        float x_exp = CLAMP(x * exp_scale, 0.0f, 1.0f);

        float s_curve = 1.0f / (1.0f + expf(-k * 8.0f * (x_exp - 0.5f)));
        float s_norm = (s_curve - s_min) / (s_max - s_min + 1e-6f);
        float blended = (1.0f - mix) * x_exp + mix * s_norm;

        if (p->black_lift > 0.0f) {
            float lift_mask = (1.0f - x_exp) * (1.0f - x_exp);
            blended += p->black_lift * lift_mask;
        }

        if (p->shoulder_strength > 0.0f) {
            float h_mask = MAX(0.0f, x_exp - 0.6f) / 0.4f;
            blended *= (1.0f - h_mask * 0.15f * p->shoulder_strength);
        }

        float curved = CLAMP(blended, 0.0f, p->white_clip);

        /* 三通道色轮权重 (B, G, R) */
        float w_s = CLAMP(1.0f - curved * 2.0f, 0.0f, 1.0f);
        float w_shadow = w_s * w_s;
        float w_h = CLAMP((curved - 0.5f) * 2.0f, 0.0f, 1.0f);
        float w_highlight = w_h * w_h;
        float w_mid = 1.0f - w_shadow - w_highlight;

        for (int ch = 0; ch < 3; ch++) {
            float delta = w_shadow * p->shadow_tint[ch] +
                          w_mid * p->mid_tint[ch] +
                          w_highlight * p->highlight_tint[ch];
            float ch_out = CLAMP(curved + delta, 0.0f, 1.0f) * 255.0f;
            out_lut3[i * 3 + ch] = clamp_u8(ch_out);
        }
    }
}

/**
 * @brief 人像肤色检测遮罩计算 (1/4 下采样图)
 */
static float compute_skin_mask_pixel(uint8_t b, uint8_t g, uint8_t r) {
    /* BGR -> YCrCb */
    int y = (299 * r + 587 * g + 114 * b + 500) / 1000;
    int cr = ((r - y) * 713 + 500) / 1000 + 128;
    int cb = ((b - y) * 564 + 500) / 1000 + 128;

    float cr_diff = ((float)cr - 150.0f) / 18.0f;
    float cb_diff = ((float)cb - 115.0f) / 15.0f;
    float dist_sq = cr_diff * cr_diff + cb_diff * cb_diff;
    float mask = expf(-0.5f * dist_sq);

    float lum_valid = CLAMP(((float)y - 40.0f) / 40.0f, 0.0f, 1.0f) *
                      CLAMP((240.0f - (float)y) / 30.0f, 0.0f, 1.0f);

    float skin = mask * lum_valid;
    return CLAMP(skin, 0.0f, 1.0f);
}

/**
 * @brief 双线性上采样遮罩并融合肤色隔离 (使用 OpenCV 规格 INTER_LINEAR)
 */
static void apply_skin_mask_protection(
    const uint8_t* clean_frame,
    uint8_t* graded_frame,
    int width,
    int height,
    int bpp,
    int small_w,
    int small_h,
    const float* small_mask
) {
    float scale_x = (float)small_w / (float)width;
    float scale_y = (float)small_h / (float)height;

    for (int y = 0; y < height; y++) {
        float sy = (y + 0.5f) * scale_y - 0.5f;
        int y0 = (int)floorf(sy);
        int y1 = MIN(y0 + 1, small_h - 1);
        if (y0 < 0) y0 = 0;
        float fy = sy - y0;

        for (int x = 0; x < width; x++) {
            float sx = (x + 0.5f) * scale_x - 0.5f;
            int x0 = (int)floorf(sx);
            int x1 = MIN(x0 + 1, small_w - 1);
            if (x0 < 0) x0 = 0;
            float fx = sx - x0;

            float m00 = small_mask[y0 * small_w + x0];
            float m10 = small_mask[y0 * small_w + x1];
            float m01 = small_mask[y1 * small_w + x0];
            float m11 = small_mask[y1 * small_w + x1];

            float mask_val = (1.0f - fx) * (1.0f - fy) * m00 +
                             fx * (1.0f - fy) * m10 +
                             (1.0f - fx) * fy * m01 +
                             fx * fy * m11;

            if (mask_val > 0.001f) {
                float alpha = 0.7f * mask_val;
                int idx = (y * width + x) * bpp;
                for (int c = 0; c < 3; c++) {
                    float orig = clean_frame[idx + c];
                    float grad = graded_frame[idx + c];
                    graded_frame[idx + c] = clamp_u8(grad * (1.0f - alpha) + orig * alpha);
                }
            }
        }
    }
}

/**
 * @brief 银盐胶片颗粒合成 (与 Python cv2.add/cv2.subtract 逻辑完全一致)
 */
static void apply_film_grain_c(
    AdaptiveGradingEngine* engine,
    uint8_t* img,
    int width,
    int height,
    int bpp,
    float grain_intensity
) {
    if (grain_intensity <= 0.005f) return;

    engine->grain_frame_idx++;
    int gw = ADAPTIVE_GRAIN_POOL_SIZE;
    int gh = ADAPTIVE_GRAIN_POOL_SIZE;

    int ox = (engine->grain_frame_idx * 37) % MAX(1, gw - MIN(width, gw) + 1);
    int oy = (engine->grain_frame_idx * 53) % MAX(1, gh - MIN(height, gh) + 1);

    int tile_w = MIN(width, gw);
    int tile_h = MIN(height, gh);

    float scale_w = (float)tile_w / width;
    float scale_h = (float)tile_h / height;
    float mult = grain_intensity * 255.0f;

    for (int y = 0; y < height; y++) {
        int py = oy + (int)(y * scale_h);
        const float* pool_row = engine->grain_pool + py * gw;
        uint8_t* img_row = img + y * width * bpp;

#if defined(__ARM_NEON) || defined(__aarch64__)
        neon_apply_film_grain_row(img_row, pool_row, ox, width, bpp, scale_w, mult);
#else
        for (int x = 0; x < width; x++) {
            int px = ox + (int)(x * scale_w);
            float noise = pool_row[px] * mult;

            uint8_t pos = (noise > 0.0f) ? (uint8_t)CLAMP(noise, 0.0f, 255.0f) : 0;
            uint8_t neg = (noise < 0.0f) ? (uint8_t)CLAMP(-noise, 0.0f, 255.0f) : 0;

            int idx = x * bpp;
            for (int c = 0; c < 3; c++) {
                int val = img_row[idx + c];
                val = MIN(val + pos, 255);
                val = MAX(val - neg, 0);
                img_row[idx + c] = (uint8_t)val;
            }
        }
#endif
    }
}

/**
 * @brief 光学暗角快速余弦合成 (与 Python cv2.multiply 保持 uint8 量化一致)
 */
static void apply_vignette_c(
    uint8_t* img,
    int width,
    int height,
    int bpp,
    float intensity
) {
    if (intensity <= 0.01f) return;

    float cx = (float)width / 2.0f;
    float cy = (float)height / 2.0f;
    float max_radius_sq = cx * cx + cy * cy;
    float half_pi = (float)(M_PI * 0.5);

    /* 512 阶径向暗角曲线查找表 (去除逐像素 92万次 sqrtf 与 cosf 极端瓶颈) */
    uint8_t lut[512];
    for (int k = 0; k < 512; k++) {
        float dist = sqrtf((float)k / 511.0f);
        float theta = CLAMP(dist * half_pi, 0.0f, half_pi);
        float mask = cosf(theta);
        float v_float = (1.0f - intensity) + intensity * mask;
        lut[k] = (uint8_t)CLAMP(v_float * 255.0f + 0.5f, 0.0f, 255.0f);
    }

    float norm_factor = 511.0f / max_radius_sq;

    for (int y = 0; y < height; y++) {
        float dy = (float)y - cy;
        float dy_norm = dy * dy * norm_factor;
        uint8_t* row = img + y * width * bpp;

        for (int x = 0; x < width; x++) {
            float dx = (float)x - cx;
            int k = (int)(dx * dx * norm_factor + dy_norm);
            if (k > 511) k = 511;
            uint32_t v_u8 = lut[k];

            int idx = x * bpp;
            row[idx + 0] = (uint8_t)((row[idx + 0] * v_u8 * 257 + 32768) >> 16);
            row[idx + 1] = (uint8_t)((row[idx + 1] * v_u8 * 257 + 32768) >> 16);
            row[idx + 2] = (uint8_t)((row[idx + 2] * v_u8 * 257 + 32768) >> 16);
        }
    }
}

/* ========================================================================== */
/* 引擎生命周期管理与主处理流程                                               */
/* ========================================================================== */

AdaptiveGradingEngine* adaptive_grading_create(int default_style_id) {
    AdaptiveGradingEngine* engine = (AdaptiveGradingEngine*)calloc(1, sizeof(AdaptiveGradingEngine));
    if (!engine) return NULL;

    if (default_style_id < 1 || default_style_id > 15) {
        default_style_id = 1;
    }
    engine->current_style_id = default_style_id;

    /* 默认时域平滑参数 */
    engine->smoother_cfg.base_alpha = 0.08f;
    engine->smoother_cfg.fast_alpha = 0.45f;
    engine->smoother_cfg.threshold = 0.15f;
    engine->smoother_initialized = 0;

    /* 初始化 1024x1024 胶片银盐颗粒池 (采用标准 Box-Muller 算法产生高斯分布，种子为 42) */
    uint32_t seed = 42;
    for (int i = 0; i < ADAPTIVE_GRAIN_POOL_SIZE * ADAPTIVE_GRAIN_POOL_SIZE; i += 2) {
        /* LCG 简易发生器配合 Box-Muller */
        seed = seed * 1664525u + 1013904223u;
        float u1 = ((seed & 0x00FFFFFFu) + 1.0f) / 16777218.0f;
        seed = seed * 1664525u + 1013904223u;
        float u2 = ((seed & 0x00FFFFFFu) + 1.0f) / 16777218.0f;

        float r = sqrtf(-2.0f * logf(u1));
        float theta = (float)(2.0 * M_PI * u2);
        engine->grain_pool[i] = r * cosf(theta);
        if (i + 1 < ADAPTIVE_GRAIN_POOL_SIZE * ADAPTIVE_GRAIN_POOL_SIZE) {
            engine->grain_pool[i + 1] = r * sinf(theta);
        }
    }

    /* 预留 1920x1080 空间 */
    engine->max_buf_width = 1920;
    engine->max_buf_height = 1080;
    int max_pixels = engine->max_buf_width * engine->max_buf_height;

    engine->temp_y = (uint8_t*)malloc(max_pixels);
    engine->temp_cr = (uint8_t*)malloc(max_pixels);
    engine->temp_cb = (uint8_t*)malloc(max_pixels);
    engine->temp_blur = (uint8_t*)malloc(max_pixels);
    engine->temp_clean_frame = (uint8_t*)malloc(max_pixels * 4);
    engine->temp_graded_frame = (uint8_t*)malloc(max_pixels * 4);
    engine->skin_mask_small = (float*)malloc((max_pixels / 16) * sizeof(float));
    engine->skin_mask_full = (float*)malloc(max_pixels * sizeof(float));

    if (!engine->temp_y || !engine->temp_clean_frame || !engine->temp_graded_frame) {
        adaptive_grading_destroy(engine);
        return NULL;
    }

    /* 初始化无锁切片并发工作线程池 (默认 4 线程) */
    engine->thread_count = 4;
    engine->thread_pool = adaptive_thread_pool_create(4);

    return engine;
}

void adaptive_grading_destroy(AdaptiveGradingEngine* engine) {
    if (!engine) return;
    if (engine->thread_pool) {
        adaptive_thread_pool_destroy(engine->thread_pool);
        engine->thread_pool = NULL;
    }
    free(engine->temp_y);
    free(engine->temp_cr);
    free(engine->temp_cb);
    free(engine->temp_blur);
    free(engine->temp_clean_frame);
    free(engine->temp_graded_frame);
    free(engine->skin_mask_small);
    free(engine->skin_mask_full);
    free(engine->vignette_mask_u8);
    free(engine);
}

AdaptiveResult adaptive_grading_set_thread_count(AdaptiveGradingEngine* engine, int num_threads) {
    if (!engine) return ADAPTIVE_ERR_NULL_POINTER;
    if (num_threads < 1) num_threads = 1;
    if (num_threads > 16) num_threads = 16;
    if (engine->thread_count == num_threads && engine->thread_pool) return ADAPTIVE_OK;

    if (engine->thread_pool) {
        adaptive_thread_pool_destroy(engine->thread_pool);
        engine->thread_pool = NULL;
    }
    engine->thread_count = num_threads;
    engine->thread_pool = adaptive_thread_pool_create(num_threads);
    return engine->thread_pool ? ADAPTIVE_OK : ADAPTIVE_ERR_OUT_OF_MEMORY;
}

int adaptive_grading_get_thread_count(const AdaptiveGradingEngine* engine) {
    if (!engine) return 1;
    return engine->thread_count;
}

AdaptiveResult adaptive_grading_set_style(AdaptiveGradingEngine* engine, int style_id) {
    if (!engine) return ADAPTIVE_ERR_NULL_POINTER;
    if (style_id < 1 || style_id > 15) return ADAPTIVE_ERR_INVALID_PARAM;
    engine->current_style_id = style_id;
    adaptive_grading_reset_smoother(engine);
    return ADAPTIVE_OK;
}

int adaptive_grading_get_style(const AdaptiveGradingEngine* engine) {
    if (!engine) return -1;
    return engine->current_style_id;
}

void adaptive_grading_reset_smoother(AdaptiveGradingEngine* engine) {
    if (engine) {
        engine->smoother_initialized = 0;
        engine->grain_frame_idx = 0;
    }
}

AdaptiveResult adaptive_grading_configure_smoother(
    AdaptiveGradingEngine* engine,
    const AdaptiveSmootherConfig* config
) {
    if (!engine || !config) return ADAPTIVE_ERR_NULL_POINTER;
    engine->smoother_cfg = *config;
    return ADAPTIVE_OK;
}

/* ========================================================================== */
/* 多线程切片着色工作函数与上下文                                             */
/* ========================================================================== */

typedef struct {
    AdaptiveGradingEngine* engine;
    const uint8_t* clean_frame;
    uint8_t* dst_data;
    int width;
    int height;
    int bpp;
    int is_rgb;
    int clean_stride;
    int dst_stride;
    const AdaptiveStyleParameters* params;
    int is_monochrome;
    const uint8_t* mono_lut;
    const uint8_t* master_lut;
    float saturation;
    int has_skin;
    int small_w;
    int small_h;
    const float* skin_mask_small;
    int has_grain;
    int ox;
    int oy;
    float scale_w;
    float scale_h;
    float grain_mult;
    int has_vignette;
    float cx;
    float cy;
    float norm_factor;
    const uint8_t* vignette_lut;
} RenderSliceContext;

static void render_slice_worker(
    void* user_data,
    int slice_idx,
    int total_slices,
    int y_start,
    int y_end
) {
    (void)slice_idx; (void)total_slices;
    RenderSliceContext* ctx = (RenderSliceContext*)user_data;
    AdaptiveGradingEngine* engine = ctx->engine;
    const uint8_t* clean_frame = ctx->clean_frame;
    uint8_t* dst_data = ctx->dst_data;
    int width = ctx->width;
    int bpp = ctx->bpp;
    int is_rgb = ctx->is_rgb;
    int clean_stride = ctx->clean_stride;
    int dst_stride = ctx->dst_stride;
    int gw = ADAPTIVE_GRAIN_POOL_SIZE;

    float scale_skin_x = (float)ctx->small_w / (float)width;
    float scale_skin_y = (float)ctx->small_h / (float)ctx->height;

    for (int y = y_start; y < y_end; y++) {
        const uint8_t* clean_row = clean_frame + y * clean_stride;
        uint8_t* out_row = dst_data + y * dst_stride;

        /* 1. 颜色映射 (黑白 或 彩色 Master LUT) */
        if (ctx->is_monochrome) {
            const uint8_t* mono_lut = ctx->mono_lut;
            for (int x = 0; x < width; x++) {
                uint32_t c0 = clean_row[x * bpp + 0];
                uint32_t c1 = clean_row[x * bpp + 1];
                uint32_t c2 = clean_row[x * bpp + 2];
                uint32_t b = is_rgb ? c2 : c0;
                uint32_t g = c1;
                uint32_t r = is_rgb ? c0 : c2;
                uint8_t curved = mono_lut[(26 * b + 115 * g + 115 * r + 128) >> 8];
                out_row[x * bpp + 0] = curved;
                out_row[x * bpp + 1] = curved;
                out_row[x * bpp + 2] = curved;
                if (bpp == 4) out_row[x * bpp + 3] = clean_row[x * bpp + 3];
            }
        } else {
            const uint8_t* lut = ctx->master_lut;
            for (int x = 0; x < width; x++) {
                uint8_t c0 = clean_row[x * bpp + 0];
                uint8_t c1 = clean_row[x * bpp + 1];
                uint8_t c2 = clean_row[x * bpp + 2];
                if (is_rgb) {
                    out_row[x * bpp + 0] = lut[c0 * 3 + 2]; /* R */
                    out_row[x * bpp + 1] = lut[c1 * 3 + 1]; /* G */
                    out_row[x * bpp + 2] = lut[c2 * 3 + 0]; /* B */
                } else {
                    out_row[x * bpp + 0] = lut[c0 * 3 + 0]; /* B */
                    out_row[x * bpp + 1] = lut[c1 * 3 + 1]; /* G */
                    out_row[x * bpp + 2] = lut[c2 * 3 + 2]; /* R */
                }
                if (bpp == 4) out_row[x * bpp + 3] = clean_row[x * bpp + 3];
        }

        /* 2. 饱和度调整 */
        if (fabsf(ctx->saturation - 1.0f) > 0.02f) {
#if defined(__ARM_NEON) || defined(__aarch64__)
            neon_apply_saturation(out_row, width, bpp, ctx->saturation);
#else
            float s = ctx->saturation;
            float wr = 0.299f, wg = 0.587f, wb = 0.114f;
            float one_minus_s = 1.0f - s;
            for (int x = 0; x < width; x++) {
                float b = is_rgb ? out_row[x * bpp + 2] : out_row[x * bpp + 0];
                float g = out_row[x * bpp + 1];
                float r = is_rgb ? out_row[x * bpp + 0] : out_row[x * bpp + 2];
                float gray = wb * b + wg * g + wr * r;
                uint8_t ob = clamp_u8(s * b + one_minus_s * gray);
                uint8_t og = clamp_u8(s * g + one_minus_s * gray);
                uint8_t or = clamp_u8(s * r + one_minus_s * gray);
                if (is_rgb) {
                    out_row[x * bpp + 0] = or;
                    out_row[x * bpp + 1] = og;
                    out_row[x * bpp + 2] = ob;
                } else {
                    out_row[x * bpp + 0] = ob;
                    out_row[x * bpp + 1] = og;
                    out_row[x * bpp + 2] = or;
                }
            }
#endif
        }

        /* 3. 肤色隔离保护融合 */
        if (ctx->has_skin) {
            float sy = ((float)y + 0.5f) * scale_skin_y - 0.5f;
            int y0 = (int)floorf(sy);
            int y1 = MIN(y0 + 1, ctx->small_h - 1);
            if (y0 < 0) y0 = 0;
            float fy = sy - (float)y0;

            for (int x = 0; x < width; x++) {
                float sx = ((float)x + 0.5f) * scale_skin_x - 0.5f;
                int x0 = (int)floorf(sx);
                int x1 = MIN(x0 + 1, ctx->small_w - 1);
                if (x0 < 0) x0 = 0;
                float fx = sx - (float)x0;

                float m00 = ctx->skin_mask_small[y0 * ctx->small_w + x0];
                float m10 = ctx->skin_mask_small[y0 * ctx->small_w + x1];
                float m01 = ctx->skin_mask_small[y1 * ctx->small_w + x0];
                float m11 = ctx->skin_mask_small[y1 * ctx->small_w + x1];

                float mask_val = (1.0f - fx) * (1.0f - fy) * m00 +
                                 fx * (1.0f - fy) * m10 +
                                 (1.0f - fx) * fy * m01 +
                                 fx * fy * m11;

                if (mask_val > 0.001f) {
                    float alpha = 0.7f * mask_val;
                    for (int c = 0; c < 3; c++) {
                        float orig = clean_row[x * bpp + c];
                        float grad = out_row[x * bpp + c];
                        out_row[x * bpp + c] = clamp_u8(grad * (1.0f - alpha) + orig * alpha);
                    }
                }
            }
        }
    }

        /* 4. 胶片颗粒质感合成 */
        if (ctx->has_grain) {
            int py = ctx->oy + (int)(y * ctx->scale_h);
            const float* pool_row = engine->grain_pool + py * gw;
#if defined(__ARM_NEON) || defined(__aarch64__)
            neon_apply_film_grain_row(
                out_row, pool_row, ctx->ox, width, bpp, ctx->scale_w, ctx->grain_mult
            );
#else
            for (int x = 0; x < width; x++) {
                int px = ctx->ox + (int)(x * ctx->scale_w);
                float noise = pool_row[px] * ctx->grain_mult;
                uint8_t pos = (noise > 0.0f) ? (uint8_t)CLAMP(noise, 0.0f, 255.0f) : 0;
                uint8_t neg = (noise < 0.0f) ? (uint8_t)CLAMP(-noise, 0.0f, 255.0f) : 0;
                int idx = x * bpp;
                for (int c = 0; c < 3; c++) {
                    int val = out_row[idx + c];
                    val = MIN(val + pos, 255);
                    val = MAX(val - neg, 0);
                    out_row[idx + c] = (uint8_t)val;
                }
            }
#endif
        }

        /* 5. 复古光学暗角合成 */
        if (ctx->has_vignette) {
            float dy = (float)y - ctx->cy;
            float dy_norm = dy * dy * ctx->norm_factor;
            const uint8_t* lut = ctx->vignette_lut;
            for (int x = 0; x < width; x++) {
                float dx = (float)x - ctx->cx;
                int k = (int)(dx * dx * ctx->norm_factor + dy_norm);
                if (k > 511) k = 511;
                uint32_t v_u8 = lut[k];
                int idx = x * bpp;
                out_row[idx + 0] = (uint8_t)((out_row[idx + 0] * v_u8 * 257 + 32768) >> 16);
                out_row[idx + 1] = (uint8_t)((out_row[idx + 1] * v_u8 * 257 + 32768) >> 16);
                out_row[idx + 2] = (uint8_t)((out_row[idx + 2] * v_u8 * 257 + 32768) >> 16);
            }
        }
    }
}

/**
 * @brief 全画质渲染管线纯 C 实现
 */
AdaptiveResult adaptive_grading_render_pipeline(
    AdaptiveGradingEngine* engine,
    const uint8_t* src_data,
    uint8_t* dst_data,
    int width,
    int height,
    int src_stride,
    int dst_stride,
    AdaptivePixelFormat format,
    const AdaptiveStyleParameters* params
) {
    if (!engine || !src_data || !dst_data || !params) return ADAPTIVE_ERR_NULL_POINTER;
    int bpp = (format == ADAPTIVE_PIXEL_FORMAT_RGBA8888 || format == ADAPTIVE_PIXEL_FORMAT_BGRA8888) ? 4 : 3;
    int total = width * height;

    /* 0. 扩容工作缓冲区 (如果超出初始 1080p 预算) */
    if (total > engine->max_buf_width * engine->max_buf_height) {
        engine->max_buf_width = width;
        engine->max_buf_height = height;
        engine->temp_y = (uint8_t*)realloc(engine->temp_y, total);
        engine->temp_cr = (uint8_t*)realloc(engine->temp_cr, total);
        engine->temp_cb = (uint8_t*)realloc(engine->temp_cb, total);
        engine->temp_blur = (uint8_t*)realloc(engine->temp_blur, total);
        engine->temp_clean_frame = (uint8_t*)realloc(engine->temp_clean_frame, total * 4);
        engine->temp_graded_frame = (uint8_t*)realloc(engine->temp_graded_frame, total * 4);
        engine->skin_mask_small = (float*)realloc(engine->skin_mask_small, (total / 16 + 1) * sizeof(float));
    }

    uint8_t* clean_frame = engine->temp_clean_frame;
    uint8_t* graded_frame = engine->temp_graded_frame;

    /* 0.1 复制输入到临时连续缓冲 */
    for (int y = 0; y < height; y++) {
        memcpy(clean_frame + y * width * bpp, src_data + y * src_stride, width * bpp);
    }

    /* 1. 画质去噪与自适应微反差 */
    enhance_clarity_and_denoise_c(
        engine,
        clean_frame,
        clean_frame,
        width,
        height,
        bpp,
        params->denoise_strength,
        params->clarity_strength
    );

    /* 2. 肤色遮罩感知 (1/4 分辨率 Area 平均下采样) */
    int has_skin = 0;
    int small_w = MAX(32, width / 4);
    int small_h = MAX(32, height / 4);

    if (params->protect_skin && !params->is_monochrome) {
        float max_skin = 0.0f;
        int step_x = width / small_w;
        int step_y = height / small_h;
        if (step_x < 1) step_x = 1;
        if (step_y < 1) step_y = 1;

        for (int y = 0; y < small_h; y++) {
            int sy = y * step_y;
            if (sy >= height) sy = height - 1;
            const uint8_t* row = clean_frame + sy * width * bpp;

            for (int x = 0; x < small_w; x++) {
                int sx = x * step_x;
                if (sx >= width) sx = width - 1;

                uint8_t b = row[sx * bpp + 0];
                uint8_t g = row[sx * bpp + 1];
                uint8_t r = row[sx * bpp + 2];
                float m = compute_skin_mask_pixel(b, g, r);
                engine->skin_mask_small[y * small_w + x] = m;
                if (m > max_skin) max_skin = m;
            }
        }
        if (max_skin > 0.08f) {
            has_skin = 1;
        }
    }

    /* 3. 预先构建 LUT 查找表与渲染参数 */
    uint8_t vignette_lut512[512];
    int has_vignette = (params->vignette > 0.01f);
    float cx = (float)width * 0.5f;
    float cy = (float)height * 0.5f;
    float max_radius_sq = cx * cx + cy * cy;
    float norm_factor = 511.0f / (max_radius_sq + 1e-6f);

    if (has_vignette) {
        float half_pi = (float)(M_PI * 0.5);
        for (int k = 0; k < 512; k++) {
            float dist = sqrtf((float)k / 511.0f);
            float theta = CLAMP(dist * half_pi, 0.0f, half_pi);
            float mask = cosf(theta);
            float v_float = (1.0f - params->vignette) + params->vignette * mask;
            vignette_lut512[k] = (uint8_t)CLAMP(v_float * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    }

    int has_grain = (params->grain_intensity > 0.005f);
    int ox = 0, oy = 0;
    float scale_w = 1.0f, scale_h = 1.0f, grain_mult = 0.0f;
    if (has_grain) {
        engine->grain_frame_idx++;
        int gw = ADAPTIVE_GRAIN_POOL_SIZE;
        int gh = ADAPTIVE_GRAIN_POOL_SIZE;
        ox = (engine->grain_frame_idx * 37) % MAX(1, gw - MIN(width, gw) + 1);
        oy = (engine->grain_frame_idx * 53) % MAX(1, gh - MIN(height, gh) + 1);
        int tile_w = MIN(width, gw);
        int tile_h = MIN(height, gh);
        scale_w = (float)tile_w / (float)width;
        scale_h = (float)tile_h / (float)height;
        grain_mult = params->grain_intensity * 255.0f;
    }

    if (params->is_monochrome) {
        build_filmic_curve_lut_c(
            params->contrast,
            params->toe_strength,
            params->shoulder_strength,
            params->black_lift,
            params->white_clip,
            engine->filmic_mono_lut
        );
    } else {
        build_master_grading_lut_c(params, engine->master_lut);
    }

    int is_rgb = (format == ADAPTIVE_PIXEL_FORMAT_RGB888 || format == ADAPTIVE_PIXEL_FORMAT_RGBA8888);

    /* 4. 组装切片渲染上下文并分派多线程并发流水线 */
    RenderSliceContext ctx;
    ctx.engine = engine;
    ctx.clean_frame = clean_frame;
    ctx.dst_data = dst_data;
    ctx.width = width;
    ctx.height = height;
    ctx.bpp = bpp;
    ctx.is_rgb = is_rgb;
    ctx.clean_stride = width * bpp;
    ctx.dst_stride = dst_stride;
    ctx.params = params;
    ctx.is_monochrome = params->is_monochrome;
    ctx.mono_lut = engine->filmic_mono_lut;
    ctx.master_lut = engine->master_lut;
    ctx.saturation = params->saturation;
    ctx.has_skin = has_skin;
    ctx.small_w = small_w;
    ctx.small_h = small_h;
    ctx.skin_mask_small = engine->skin_mask_small;
    ctx.has_grain = has_grain;
    ctx.ox = ox;
    ctx.oy = oy;
    ctx.scale_w = scale_w;
    ctx.scale_h = scale_h;
    ctx.grain_mult = grain_mult;
    ctx.has_vignette = has_vignette;
    ctx.cx = cx;
    ctx.cy = cy;
    ctx.norm_factor = norm_factor;
    ctx.vignette_lut = vignette_lut512;

    adaptive_thread_pool_dispatch(engine->thread_pool, height, render_slice_worker, &ctx);

    return ADAPTIVE_OK;
}

AdaptiveResult adaptive_grading_process_frame(
    AdaptiveGradingEngine* engine,
    const uint8_t* src_data,
    uint8_t* dst_data,
    int width,
    int height,
    int src_stride,
    int dst_stride,
    AdaptivePixelFormat format,
    int split_mode,
    float split_ratio,
    AdaptiveSceneMetrics* out_metrics,
    AdaptiveStyleParameters* out_active_params
) {
    if (!engine || !src_data || !dst_data || width <= 0 || height <= 0) {
        return ADAPTIVE_ERR_INVALID_PARAM;
    }

    /* 1. 场景分析感知 */
    AdaptiveSceneMetrics metrics;
    AdaptiveResult res = adaptive_grading_analyze(src_data, width, height, src_stride, format, &metrics);
    if (res != ADAPTIVE_OK) return res;

    /* 2. 风格解算 */
    AdaptiveStyleParameters raw_params;
    res = adaptive_grading_solve_style(engine->current_style_id, &metrics, &raw_params);
    if (res != ADAPTIVE_OK) return res;

    /* 3. 时域平滑 */
    AdaptiveStyleParameters active_params;
    smoother_update_all(engine, &raw_params, &active_params);

    /* 4. 全画质着色渲染 */
    res = adaptive_grading_render_pipeline(
        engine,
        src_data,
        dst_data,
        width,
        height,
        src_stride,
        dst_stride,
        format,
        &active_params
    );
    if (res != ADAPTIVE_OK) return res;

    /* 5. 分屏对比处理 */
    if (split_mode) {
        int bpp = (format == ADAPTIVE_PIXEL_FORMAT_RGBA8888 || format == ADAPTIVE_PIXEL_FORMAT_BGRA8888) ? 4 : 3;
        int split_x = (int)(width * CLAMP(split_ratio, 0.0f, 1.0f));

        for (int y = 0; y < height; y++) {
            const uint8_t* s_row = src_data + y * src_stride;
            uint8_t* d_row = dst_data + y * dst_stride;

            /* 左侧原图 */
            if (split_x > 0) {
                memcpy(d_row, s_row, split_x * bpp);
            }

            /* 白色分割线 (2 像素宽) */
            if (split_x < width) {
                for (int c = 0; c < 3; c++) d_row[split_x * bpp + c] = 255;
            }
            if (split_x + 1 < width) {
                for (int c = 0; c < 3; c++) d_row[(split_x + 1) * bpp + c] = 255;
            }
        }
    }

    if (out_metrics) *out_metrics = metrics;
    if (out_active_params) *out_active_params = active_params;

    return ADAPTIVE_OK;
}

AdaptiveResult adaptive_grading_process_yuv420(
    AdaptiveGradingEngine* engine,
    const uint8_t* y_plane,
    const uint8_t* uv_plane,
    uint8_t* dst_rgba,
    int width,
    int height,
    int y_stride,
    int uv_stride,
    int dst_stride,
    int is_nv21,
    int split_mode,
    float split_ratio,
    AdaptiveSceneMetrics* out_metrics,
    AdaptiveStyleParameters* out_active_params
) {
    if (!engine || !y_plane || !uv_plane || !dst_rgba || width <= 0 || height <= 0) {
        return ADAPTIVE_ERR_INVALID_PARAM;
    }

    /* 1. 毫秒级场景分析: 从 Y/UV 平面下采样 256x256 缩略图并分析 */
    int step_x = width / ADAPTIVE_ANALYZER_TARGET_SIZE;
    int step_y = height / ADAPTIVE_ANALYZER_TARGET_SIZE;
    if (step_x < 1) step_x = 1;
    if (step_y < 1) step_y = 1;

    uint8_t* thumb = engine->thumb_bgr;
    for (int ty = 0; ty < ADAPTIVE_ANALYZER_TARGET_SIZE; ty++) {
        int sy = ty * step_y;
        if (sy >= height) sy = height - 1;
        const uint8_t* y_row = y_plane + sy * y_stride;
        const uint8_t* uv_row = uv_plane + (sy >> 1) * uv_stride;

        for (int tx = 0; tx < ADAPTIVE_ANALYZER_TARGET_SIZE; tx++) {
            int sx = tx * step_x;
            if (sx >= width) sx = width - 1;

            int y_val = y_row[sx];
            int u, v;
            if (is_nv21) {
                v = uv_row[(sx >> 1) * 2 + 0];
                u = uv_row[(sx >> 1) * 2 + 1];
            } else {
                u = uv_row[(sx >> 1) * 2 + 0];
                v = uv_row[(sx >> 1) * 2 + 1];
            }

            int c = y_val - 16;
            int d = u - 128;
            int e = v - 128;
            if (c < 0) c = 0;

            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            int idx = (ty * ADAPTIVE_ANALYZER_TARGET_SIZE + tx) * 3;
            thumb[idx + 0] = clamp_i32_u8(b);
            thumb[idx + 1] = clamp_i32_u8(g);
            thumb[idx + 2] = clamp_i32_u8(r);
        }
    }

    AdaptiveSceneMetrics metrics;
    AdaptiveResult res = adaptive_grading_analyze(
        thumb,
        ADAPTIVE_ANALYZER_TARGET_SIZE,
        ADAPTIVE_ANALYZER_TARGET_SIZE,
        ADAPTIVE_ANALYZER_TARGET_SIZE * 3,
        ADAPTIVE_PIXEL_FORMAT_BGR888,
        &metrics
    );
    if (res != ADAPTIVE_OK) return res;

    /* 2. 风格解算 */
    AdaptiveStyleParameters raw_params;
    res = adaptive_grading_solve_style(engine->current_style_id, &metrics, &raw_params);
    if (res != ADAPTIVE_OK) return res;

    /* 3. 时域平滑 */
    AdaptiveStyleParameters active_params;
    smoother_update_all(engine, &raw_params, &active_params);

    /* 实时相机流模式：去噪与边缘锐化已由手机硬件 ISP (Snapdragon Spectra ISP) 处理，
     * 旁路重负荷 CPU 软件高斯模糊以达成极速 60 FPS+ (< 3ms) */
    active_params.denoise_strength = 0.0f;
    active_params.clarity_strength = 0.0f;

    /* 4. 相机原生 YUV420 -> RGBA 转换 (硬件 NEON 加速) */
#if defined(__ARM_NEON) || defined(__aarch64__)
    neon_yuv420_to_rgba(
        y_plane, uv_plane, dst_rgba, width, height, y_stride, uv_stride, dst_stride, is_nv21
    );
#else
    for (int y = 0; y < height; y++) {
        const uint8_t* y_row = y_plane + y * y_stride;
        const uint8_t* uv_row = uv_plane + (y >> 1) * uv_stride;
        uint8_t* d_row = dst_rgba + y * dst_stride;

        for (int x = 0; x < width; x++) {
            int y_val = y_row[x];
            int u, v;
            if (is_nv21) {
                v = uv_row[(x >> 1) * 2 + 0];
                u = uv_row[(x >> 1) * 2 + 1];
            } else {
                u = uv_row[(x >> 1) * 2 + 0];
                v = uv_row[(x >> 1) * 2 + 1];
            }

            int c = y_val - 16;
            int d = u - 128;
            int e = v - 128;
            if (c < 0) c = 0;

            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            d_row[x * 4 + 0] = clamp_i32_u8(r);
            d_row[x * 4 + 1] = clamp_i32_u8(g);
            d_row[x * 4 + 2] = clamp_i32_u8(b);
            d_row[x * 4 + 3] = 255;
        }
    }
#endif

    /* 5. 原地执行全画质切片着色流水线 (dst_rgba -> dst_rgba) */
    res = adaptive_grading_render_pipeline(
        engine,
        dst_rgba,
        dst_rgba,
        width,
        height,
        dst_stride,
        dst_stride,
        ADAPTIVE_PIXEL_FORMAT_RGBA8888,
        &active_params
    );
    if (res != ADAPTIVE_OK) return res;

    /* 6. 可选分屏对比处理 (匹配手机 90° 旋转后的竖屏左右分屏) */
    if (split_mode) {
        int split_y = (int)(height * (1.0f - CLAMP(split_ratio, 0.0f, 1.0f)));
        for (int y = split_y; y < height; y++) {
            const uint8_t* y_row = y_plane + y * y_stride;
            const uint8_t* uv_row = uv_plane + (y >> 1) * uv_stride;
            uint8_t* d_row = dst_rgba + y * dst_stride;

            for (int x = 0; x < width; x++) {
                int y_val = y_row[x];
                int u = is_nv21 ? uv_row[(x >> 1) * 2 + 1] : uv_row[(x >> 1) * 2 + 0];
                int v = is_nv21 ? uv_row[(x >> 1) * 2 + 0] : uv_row[(x >> 1) * 2 + 1];
                int c = MAX(0, y_val - 16);
                int d = u - 128;
                int e = v - 128;
                d_row[x * 4 + 0] = clamp_i32_u8((298 * c + 409 * e + 128) >> 8);
                d_row[x * 4 + 1] = clamp_i32_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
                d_row[x * 4 + 2] = clamp_i32_u8((298 * c + 516 * d + 128) >> 8);
                d_row[x * 4 + 3] = 255;
            }
        }
        if (split_y >= 0 && split_y < height) {
            memset(dst_rgba + split_y * dst_stride, 255, width * 4);
        }
        if (split_y + 1 >= 0 && split_y + 1 < height) {
            memset(dst_rgba + (split_y + 1) * dst_stride, 255, width * 4);
        }
    }

    if (out_metrics) *out_metrics = metrics;
    if (out_active_params) *out_active_params = active_params;

    return ADAPTIVE_OK;
}
