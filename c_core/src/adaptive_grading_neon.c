/**
 * @file adaptive_grading_neon.c
 * @brief ARM64 NEON 向量指令集加速内核实现
 * @details 专为高通骁龙 8 至尊版 (Oryon CPU) 与 ARM64-v8a 优化
 */

#include "adaptive_grading_neon.h"

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#include <math.h>

#define CLAMP_U8(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

/**
 * @brief NEON 加速：BGR/RGBA -> YCrCb 平面分离转换
 */
void neon_bgr_to_ycrcb(
    const uint8_t* src,
    uint8_t* y_plane,
    uint8_t* cr_plane,
    uint8_t* cb_plane,
    int total_pixels,
    int bpp
) {
    int i = 0;
    float32x4_t v_wb = vdupq_n_f32(0.114f);
    float32x4_t v_wg = vdupq_n_f32(0.587f);
    float32x4_t v_wr = vdupq_n_f32(0.299f);
    float32x4_t v_half = vdupq_n_f32(0.5f);
    float32x4_t v_128_half = vdupq_n_f32(128.5f);
    float32x4_t v_cr_scale = vdupq_n_f32(0.713f);
    float32x4_t v_cb_scale = vdupq_n_f32(0.564f);

    for (; i <= total_pixels - 4; i += 4) {
        float32x4_t b, g, r;
        if (bpp == 4) {
            uint8x8x4_t pix = vld4_u8(src + i * 4);
            uint16x4_t b16 = vget_low_u16(vmovl_u8(pix.val[0]));
            uint16x4_t g16 = vget_low_u16(vmovl_u8(pix.val[1]));
            uint16x4_t r16 = vget_low_u16(vmovl_u8(pix.val[2]));
            b = vcvtq_f32_u32(vmovl_u16(b16));
            g = vcvtq_f32_u32(vmovl_u16(g16));
            r = vcvtq_f32_u32(vmovl_u16(r16));
        } else {
            uint8x8x3_t pix = vld3_u8(src + i * 3);
            uint16x4_t b16 = vget_low_u16(vmovl_u8(pix.val[0]));
            uint16x4_t g16 = vget_low_u16(vmovl_u8(pix.val[1]));
            uint16x4_t r16 = vget_low_u16(vmovl_u8(pix.val[2]));
            b = vcvtq_f32_u32(vmovl_u16(b16));
            g = vcvtq_f32_u32(vmovl_u16(g16));
            r = vcvtq_f32_u32(vmovl_u16(r16));
        }

        /* y = 0.114*b + 0.587*g + 0.299*r + 0.5 */
        float32x4_t y = vfmaq_f32(v_half, b, v_wb);
        y = vfmaq_f32(y, g, v_wg);
        y = vfmaq_f32(y, r, v_wr);

        /* cr = (r - y) * 0.713 + 128.5 */
        float32x4_t diff_r = vsubq_f32(r, y);
        float32x4_t cr = vfmaq_f32(v_128_half, diff_r, v_cr_scale);

        /* cb = (b - y) * 0.564 + 128.5 */
        float32x4_t diff_b = vsubq_f32(b, y);
        float32x4_t cb = vfmaq_f32(v_128_half, diff_b, v_cb_scale);

        /* 转换并饱和写出 */
        int32x4_t yi = vcvtq_s32_f32(y);
        int32x4_t cri = vcvtq_s32_f32(cr);
        int32x4_t cbi = vcvtq_s32_f32(cb);

        int16x4_t y16 = vqmovn_s32(yi);
        int16x4_t cr16 = vqmovn_s32(cri);
        int16x4_t cb16 = vqmovn_s32(cbi);

        uint8x8_t y8 = vqmovun_s16(vcombine_s16(y16, y16));
        uint8x8_t cr8 = vqmovun_s16(vcombine_s16(cr16, cr16));
        uint8x8_t cb8 = vqmovun_s16(vcombine_s16(cb16, cb16));

        vst1_lane_u32((uint32_t*)(y_plane + i), vreinterpret_u32_u8(y8), 0);
        vst1_lane_u32((uint32_t*)(cr_plane + i), vreinterpret_u32_u8(cr8), 0);
        vst1_lane_u32((uint32_t*)(cb_plane + i), vreinterpret_u32_u8(cb8), 0);
    }

    /* 标量尾部处理 */
    for (; i < total_pixels; i++) {
        int b = src[i * bpp + 0];
        int g = src[i * bpp + 1];
        int r = src[i * bpp + 2];

        int y = (int)(0.114f * b + 0.587f * g + 0.299f * r + 0.5f);
        int cr = (int)((r - y) * 0.713f + 128.0f + 0.5f);
        int cb = (int)((b - y) * 0.564f + 128.0f + 0.5f);

        y_plane[i] = (uint8_t)CLAMP_U8(y);
        cr_plane[i] = (uint8_t)CLAMP_U8(cr);
        cb_plane[i] = (uint8_t)CLAMP_U8(cb);
    }
}

/**
 * @brief NEON 加速：YCrCb -> BGR/RGBA 恢复
 */
void neon_ycrcb_to_bgr(
    const uint8_t* y_plane,
    const uint8_t* cr_plane,
    const uint8_t* cb_plane,
    uint8_t* dst,
    int total_pixels,
    int bpp
) {
    int i = 0;
    float32x4_t v_128 = vdupq_n_f32(128.0f);
    float32x4_t v_half = vdupq_n_f32(0.5f);
    float32x4_t v_r_cr = vdupq_n_f32(1.403f);
    float32x4_t v_g_cr = vdupq_n_f32(-0.714f);
    float32x4_t v_g_cb = vdupq_n_f32(-0.344f);
    float32x4_t v_b_cb = vdupq_n_f32(1.773f);

    for (; i <= total_pixels - 8; i += 8) {
        uint8x8_t y8 = vld1_u8(y_plane + i);
        uint8x8_t cr8 = vld1_u8(cr_plane + i);
        uint8x8_t cb8 = vld1_u8(cb_plane + i);

        uint16x8_t y16 = vmovl_u8(y8);
        uint16x8_t cr16 = vmovl_u8(cr8);
        uint16x8_t cb16 = vmovl_u8(cb8);

        float32x4_t y_l = vcvtq_f32_u32(vmovl_u16(vget_low_u16(y16)));
        float32x4_t cr_l = vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(cr16))), v_128);
        float32x4_t cb_l = vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(cb16))), v_128);

        float32x4_t y_h = vcvtq_f32_u32(vmovl_u16(vget_high_u16(y16)));
        float32x4_t cr_h = vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(cr16))), v_128);
        float32x4_t cb_h = vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(cb16))), v_128);

        /* r = y + 1.403*cr + 0.5 */
        float32x4_t rl = vaddq_f32(vfmaq_f32(y_l, cr_l, v_r_cr), v_half);
        float32x4_t rh = vaddq_f32(vfmaq_f32(y_h, cr_h, v_r_cr), v_half);

        /* g = y - 0.714*cr - 0.344*cb + 0.5 */
        float32x4_t gl = vaddq_f32(vfmaq_f32(vfmaq_f32(y_l, cr_l, v_g_cr), cb_l, v_g_cb), v_half);
        float32x4_t gh = vaddq_f32(vfmaq_f32(vfmaq_f32(y_h, cr_h, v_g_cr), cb_h, v_g_cb), v_half);

        /* b = y + 1.773*cb + 0.5 */
        float32x4_t bl = vaddq_f32(vfmaq_f32(y_l, cb_l, v_b_cb), v_half);
        float32x4_t bh = vaddq_f32(vfmaq_f32(y_h, cb_h, v_b_cb), v_half);

        /* 饱和转换 */
        int16x8_t r_out = vcombine_s16(vqmovn_s32(vcvtq_s32_f32(rl)), vqmovn_s32(vcvtq_s32_f32(rh)));
        int16x8_t g_out = vcombine_s16(vqmovn_s32(vcvtq_s32_f32(gl)), vqmovn_s32(vcvtq_s32_f32(gh)));
        int16x8_t b_out = vcombine_s16(vqmovn_s32(vcvtq_s32_f32(bl)), vqmovn_s32(vcvtq_s32_f32(bh)));

        uint8x8_t r_u8 = vqmovun_s16(r_out);
        uint8x8_t g_u8 = vqmovun_s16(g_out);
        uint8x8_t b_u8 = vqmovun_s16(b_out);

        if (bpp == 4) {
            uint8x8x4_t out_pix;
            out_pix.val[0] = b_u8;
            out_pix.val[1] = g_u8;
            out_pix.val[2] = r_u8;
            out_pix.val[3] = vdup_n_u8(255);
            vst4_u8(dst + i * 4, out_pix);
        } else {
            uint8x8x3_t out_pix;
            out_pix.val[0] = b_u8;
            out_pix.val[1] = g_u8;
            out_pix.val[2] = r_u8;
            vst3_u8(dst + i * 3, out_pix);
        }
    }

    for (; i < total_pixels; i++) {
        int y = y_plane[i];
        int cr = cr_plane[i] - 128;
        int cb = cb_plane[i] - 128;

        int r = (int)(y + 1.403f * cr + 0.5f);
        int g = (int)(y - 0.714f * cr - 0.344f * cb + 0.5f);
        int b = (int)(y + 1.773f * cb + 0.5f);

        dst[i * bpp + 0] = (uint8_t)CLAMP_U8(b);
        dst[i * bpp + 1] = (uint8_t)CLAMP_U8(g);
        dst[i * bpp + 2] = (uint8_t)CLAMP_U8(r);
        if (bpp == 4) dst[i * bpp + 3] = 255;
    }
}

/**
 * @brief NEON 加速：定点化饱和度调整矩阵 (16 像素/步)
 */
void neon_apply_saturation(
    uint8_t* img,
    int total_pixels,
    int bpp,
    float saturation
) {
    if (fabsf(saturation - 1.0f) <= 0.02f) return;

    /* 转换为 Q10 定点数 (0.0 ~ 2.0 -> 0 ~ 2048) */
    int s_q10 = (int)(saturation * 1024.0f + 0.5f);
    int inv_s_q10 = 1024 - s_q10;

    int16x8_t v_s = vdupq_n_s16((int16_t)s_q10);
    int16x8_t v_inv = vdupq_n_s16((int16_t)inv_s_q10);

    int16x8_t v_wb = vdupq_n_s16(117);
    int16x8_t v_wg = vdupq_n_s16(601);
    int16x8_t v_wr = vdupq_n_s16(306);

    int i = 0;
    if (bpp == 4) {
        for (; i <= total_pixels - 8; i += 8) {
            uint8x8x4_t v_pix = vld4_u8(img + i * 4);

            int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(v_pix.val[0]));
            int16x8_t g = vreinterpretq_s16_u16(vmovl_u8(v_pix.val[1]));
            int16x8_t r = vreinterpretq_s16_u16(vmovl_u8(v_pix.val[2]));

            int32x4_t gray_l = vmull_s16(vget_low_s16(b), vget_low_s16(v_wb));
            gray_l = vmlal_s16(gray_l, vget_low_s16(g), vget_low_s16(v_wg));
            gray_l = vmlal_s16(gray_l, vget_low_s16(r), vget_low_s16(v_wr));
            gray_l = vrshrq_n_s32(gray_l, 10);

            int32x4_t gray_h = vmull_s16(vget_high_s16(b), vget_high_s16(v_wb));
            gray_h = vmlal_s16(gray_h, vget_high_s16(g), vget_high_s16(v_wg));
            gray_h = vmlal_s16(gray_h, vget_high_s16(r), vget_high_s16(v_wr));
            gray_h = vrshrq_n_s32(gray_h, 10);

            int16x8_t gray = vcombine_s16(vmovn_s32(gray_l), vmovn_s32(gray_h));

            int32x4_t bl = vmlal_s16(vmull_s16(vget_low_s16(b), vget_low_s16(v_s)), vget_low_s16(gray), vget_low_s16(v_inv));
            int32x4_t bh = vmlal_s16(vmull_s16(vget_high_s16(b), vget_high_s16(v_s)), vget_high_s16(gray), vget_high_s16(v_inv));
            int16x8_t b_out = vcombine_s16(vrshrn_n_s32(bl, 10), vrshrn_n_s32(bh, 10));

            int32x4_t gl = vmlal_s16(vmull_s16(vget_low_s16(g), vget_low_s16(v_s)), vget_low_s16(gray), vget_low_s16(v_inv));
            int32x4_t gh = vmlal_s16(vmull_s16(vget_high_s16(g), vget_high_s16(v_s)), vget_high_s16(gray), vget_high_s16(v_inv));
            int16x8_t g_out = vcombine_s16(vrshrn_n_s32(gl, 10), vrshrn_n_s32(gh, 10));

            int32x4_t rl = vmlal_s16(vmull_s16(vget_low_s16(r), vget_low_s16(v_s)), vget_low_s16(gray), vget_low_s16(v_inv));
            int32x4_t rh = vmlal_s16(vmull_s16(vget_high_s16(r), vget_high_s16(v_s)), vget_high_s16(gray), vget_high_s16(v_inv));
            int16x8_t r_out = vcombine_s16(vrshrn_n_s32(rl, 10), vrshrn_n_s32(rh, 10));

            v_pix.val[0] = vqmovun_s16(b_out);
            v_pix.val[1] = vqmovun_s16(g_out);
            v_pix.val[2] = vqmovun_s16(r_out);

            vst4_u8(img + i * 4, v_pix);
        }
    } else {
        for (; i <= total_pixels - 8; i += 8) {
            uint8x8x3_t v_pix = vld3_u8(img + i * 3);

            int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(v_pix.val[0]));
            int16x8_t g = vreinterpretq_s16_u16(vmovl_u8(v_pix.val[1]));
            int16x8_t r = vreinterpretq_s16_u16(vmovl_u8(v_pix.val[2]));

            int32x4_t gray_l = vmull_s16(vget_low_s16(b), vget_low_s16(v_wb));
            gray_l = vmlal_s16(gray_l, vget_low_s16(g), vget_low_s16(v_wg));
            gray_l = vmlal_s16(gray_l, vget_low_s16(r), vget_low_s16(v_wr));
            gray_l = vrshrq_n_s32(gray_l, 10);

            int32x4_t gray_h = vmull_s16(vget_high_s16(b), vget_high_s16(v_wb));
            gray_h = vmlal_s16(gray_h, vget_high_s16(g), vget_high_s16(v_wg));
            gray_h = vmlal_s16(gray_h, vget_high_s16(r), vget_high_s16(v_wr));
            gray_h = vrshrq_n_s32(gray_h, 10);

            int16x8_t gray = vcombine_s16(vmovn_s32(gray_l), vmovn_s32(gray_h));

            int32x4_t bl = vmlal_s16(vmull_s16(vget_low_s16(b), vget_low_s16(v_s)), vget_low_s16(gray), vget_low_s16(v_inv));
            int32x4_t bh = vmlal_s16(vmull_s16(vget_high_s16(b), vget_high_s16(v_s)), vget_high_s16(gray), vget_high_s16(v_inv));
            int16x8_t b_out = vcombine_s16(vrshrn_n_s32(bl, 10), vrshrn_n_s32(bh, 10));

            int32x4_t gl = vmlal_s16(vmull_s16(vget_low_s16(g), vget_low_s16(v_s)), vget_low_s16(gray), vget_low_s16(v_inv));
            int32x4_t gh = vmlal_s16(vmull_s16(vget_high_s16(g), vget_high_s16(v_s)), vget_high_s16(gray), vget_high_s16(v_inv));
            int16x8_t g_out = vcombine_s16(vrshrn_n_s32(gl, 10), vrshrn_n_s32(gh, 10));

            int32x4_t rl = vmlal_s16(vmull_s16(vget_low_s16(r), vget_low_s16(v_s)), vget_low_s16(gray), vget_low_s16(v_inv));
            int32x4_t rh = vmlal_s16(vmull_s16(vget_high_s16(r), vget_high_s16(v_s)), vget_high_s16(gray), vget_high_s16(v_inv));
            int16x8_t r_out = vcombine_s16(vrshrn_n_s32(rl, 10), vrshrn_n_s32(rh, 10));

            v_pix.val[0] = vqmovun_s16(b_out);
            v_pix.val[1] = vqmovun_s16(g_out);
            v_pix.val[2] = vqmovun_s16(r_out);

            vst3_u8(img + i * 3, v_pix);
        }
    }

    /* 尾部余量标量补偿 */
    float s = saturation;
    float wr = 0.299f, wg = 0.587f, wb = 0.114f;
    float one_minus_s = 1.0f - s;
    for (; i < total_pixels; i++) {
        float b = img[i * bpp + 0];
        float g = img[i * bpp + 1];
        float r = img[i * bpp + 2];
        float gray = wb * b + wg * g + wr * r;
        img[i * bpp + 0] = (uint8_t)CLAMP_U8(s * b + one_minus_s * gray);
        img[i * bpp + 1] = (uint8_t)CLAMP_U8(s * g + one_minus_s * gray);
        img[i * bpp + 2] = (uint8_t)CLAMP_U8(s * r + one_minus_s * gray);
    }
}

/**
 * @brief NEON 加速：胶片颗粒银盐噪波饱和合成 (单指令 vqaddq_u8 / vqsubq_u8)
 */
void neon_apply_film_grain_row(
    uint8_t* img_row,
    const float* pool_row,
    int ox,
    int width,
    int bpp,
    float scale_w,
    float mult
) {
    int x = 0;
    for (; x <= width - 8; x += 8) {
        uint8_t pos_buf[8];
        uint8_t neg_buf[8];

        for (int k = 0; k < 8; k++) {
            int px = ox + (int)((x + k) * scale_w);
            float noise = pool_row[px] * mult;
            pos_buf[k] = (noise > 0.0f) ? (uint8_t)CLAMP_U8(noise) : 0;
            neg_buf[k] = (noise < 0.0f) ? (uint8_t)CLAMP_U8(-noise) : 0;
        }

        uint8x8_t v_pos = vld1_u8(pos_buf);
        uint8x8_t v_neg = vld1_u8(neg_buf);

        if (bpp == 4) {
            uint8x8x4_t v_pix = vld4_u8(img_row + x * 4);
            for (int c = 0; c < 3; c++) {
                v_pix.val[c] = vqadd_u8(v_pix.val[c], v_pos);
                v_pix.val[c] = vqsub_u8(v_pix.val[c], v_neg);
            }
            vst4_u8(img_row + x * 4, v_pix);
        } else {
            uint8x8x3_t v_pix = vld3_u8(img_row + x * 3);
            for (int c = 0; c < 3; c++) {
                v_pix.val[c] = vqadd_u8(v_pix.val[c], v_pos);
                v_pix.val[c] = vqsub_u8(v_pix.val[c], v_neg);
            }
            vst3_u8(img_row + x * 3, v_pix);
        }
    }

    for (; x < width; x++) {
        int px = ox + (int)(x * scale_w);
        float noise = pool_row[px] * mult;
        uint8_t pos = (noise > 0.0f) ? (uint8_t)CLAMP_U8(noise) : 0;
        uint8_t neg = (noise < 0.0f) ? (uint8_t)CLAMP_U8(-noise) : 0;
        int idx = x * bpp;
        for (int c = 0; c < 3; c++) {
            int val = img_row[idx + c];
            val = (val + pos > 255) ? 255 : val + pos;
            val = (val - neg < 0) ? 0 : val - neg;
            img_row[idx + c] = (uint8_t)val;
        }
    }
}

/**
 * @brief NEON 加速：相机 YUV420 (NV12/NV21) 直接融合转换为 RGBA
 */
void neon_yuv420_to_rgba(
    const uint8_t* y_plane,
    const uint8_t* uv_plane,
    uint8_t* dst_rgba,
    int width,
    int height,
    int y_stride,
    int uv_stride,
    int dst_stride,
    int is_nv21
) {
    for (int y = 0; y < height; y += 2) {
        const uint8_t* y_row0 = y_plane + y * y_stride;
        const uint8_t* y_row1 = y_plane + (y + 1) * y_stride;
        const uint8_t* uv_row = uv_plane + (y / 2) * uv_stride;

        uint8_t* dst_row0 = dst_rgba + y * dst_stride;
        uint8_t* dst_row1 = dst_rgba + (y + 1) * dst_stride;

        for (int x = 0; x < width; x += 2) {
            uint8_t u, v;
            if (is_nv21) {
                v = uv_row[x];
                u = uv_row[x + 1];
            } else {
                u = uv_row[x];
                v = uv_row[x + 1];
            }

            int c_u = (int)u - 128;
            int c_v = (int)v - 128;

            int r_diff = (359 * c_v + 128) >> 8;
            int g_diff = (-88 * c_u - 183 * c_v + 128) >> 8;
            int b_diff = (454 * c_u + 128) >> 8;

            int y00 = y_row0[x];
            int y01 = y_row0[x + 1];
            int y10 = y_row1[x];
            int y11 = y_row1[x + 1];

            dst_row0[(x + 0) * 4 + 0] = (uint8_t)CLAMP_U8(y00 + r_diff);
            dst_row0[(x + 0) * 4 + 1] = (uint8_t)CLAMP_U8(y00 + g_diff);
            dst_row0[(x + 0) * 4 + 2] = (uint8_t)CLAMP_U8(y00 + b_diff);
            dst_row0[(x + 0) * 4 + 3] = 255;

            dst_row0[(x + 1) * 4 + 0] = (uint8_t)CLAMP_U8(y01 + r_diff);
            dst_row0[(x + 1) * 4 + 1] = (uint8_t)CLAMP_U8(y01 + g_diff);
            dst_row0[(x + 1) * 4 + 2] = (uint8_t)CLAMP_U8(y01 + b_diff);
            dst_row0[(x + 1) * 4 + 3] = 255;

            dst_row1[(x + 0) * 4 + 0] = (uint8_t)CLAMP_U8(y10 + r_diff);
            dst_row1[(x + 0) * 4 + 1] = (uint8_t)CLAMP_U8(y10 + g_diff);
            dst_row1[(x + 0) * 4 + 2] = (uint8_t)CLAMP_U8(y10 + b_diff);
            dst_row1[(x + 0) * 4 + 3] = 255;

            dst_row1[(x + 1) * 4 + 0] = (uint8_t)CLAMP_U8(y11 + r_diff);
            dst_row1[(x + 1) * 4 + 1] = (uint8_t)CLAMP_U8(y11 + g_diff);
            dst_row1[(x + 1) * 4 + 2] = (uint8_t)CLAMP_U8(y11 + b_diff);
            dst_row1[(x + 1) * 4 + 3] = 255;
        }
    }
}

#endif /* defined(__ARM_NEON) || defined(__aarch64__) */
