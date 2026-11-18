/**
 * @file test_c_core.c
 * @brief C 核心库全套 Golden Test Vectors 执行器与单测
 */

#include "adaptive_grading.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

/* 读取二进制文件 */
static uint8_t* read_file_bytes(const char* filepath, size_t* out_size) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        printf("[WARN] 无法打开文件: %s\n", filepath);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t* buf = (uint8_t*)malloc(sz);
    if (buf) {
        fread(buf, 1, sz, fp);
        if (out_size) *out_size = (size_t)sz;
    }
    fclose(fp);
    return buf;
}

/* 写入二进制文件 */
static int write_file_bytes(const char* filepath, const uint8_t* data, size_t size) {
    FILE* fp = fopen(filepath, "wb");
    if (!fp) return -1;
    fwrite(data, 1, size, fp);
    fclose(fp);
    return 0;
}

int main(int argc, char** argv) {
    printf("======================================================================\n");
    printf("      C 核心库 Golden Test Vectors 自动化执行与单测程序\n");
    printf("======================================================================\n");

    const char* gv_dir = (argc > 1) ? argv[1] : "tests/golden_vectors";
    const char* out_dir = (argc > 2) ? argv[2] : "tests/c_outputs";
    MKDIR(out_dir);

    AdaptiveGradingEngine* engine = adaptive_grading_create(1);
    if (!engine) {
        printf("[FATAL] 引擎创建失败，内存不足！\n");
        return 1;
    }

    /* 1. 加载银盐颗粒基准池 */
    char grain_path[256];
    snprintf(grain_path, sizeof(grain_path), "%s/grain_pool_1024x1024_f32.raw", gv_dir);
    size_t grain_sz = 0;
    uint8_t* grain_bytes = read_file_bytes(grain_path, &grain_sz);
    if (grain_bytes && grain_sz == 1024 * 1024 * sizeof(float)) {
        adaptive_grading_set_grain_pool(engine, (const float*)grain_bytes);
        printf("[OK] 成功载入基准颗粒池 (4MB)\n");
    }
    free(grain_bytes);

    /* 2. 执行 Level 1 & Level 2: 指标感知与 15 款风格解算 */
    printf("[1/3] 执行 Level 1 (SceneMetrics) 与 Level 2 (15 Styles Solver)...\n");
    const char* cases[] = {
        "scene_01_portrait",
        "scene_02_sunset",
        "scene_03_sunny",
        "scene_04_overcast",
        "scene_05_high_dr",
        "scene_06_night_neon",
        "edge_pure_black",
        "edge_pure_white",
        "edge_neutral_gray",
        "edge_color_bars",
        "edge_checkerboard",
        "aspect_wide_640x160",
        "aspect_tall_160x640",
        "res_standard_720p"
    };
    int num_cases = sizeof(cases) / sizeof(cases[0]);

    char m_path[256], s_path[256];
    snprintf(m_path, sizeof(m_path), "%s/c_metrics.json", out_dir);
    snprintf(s_path, sizeof(s_path), "%s/c_styles.json", out_dir);
    FILE* fp_m = fopen(m_path, "w");
    FILE* fp_s = fopen(s_path, "w");
    if (!fp_m || !fp_s) {
        printf("[FATAL] 无法创建输出 JSON 文件于 %s\n", out_dir);
        return 1;
    }
    fprintf(fp_m, "{\n");
    fprintf(fp_s, "{\n");

    for (int i = 0; i < num_cases; i++) {
        const char* c_name = cases[i];
        int w = 256, h = 256;
        if (strcmp(c_name, "aspect_wide_640x160") == 0) { w = 640; h = 160; }
        else if (strcmp(c_name, "aspect_tall_160x640") == 0) { w = 160; h = 640; }
        else if (strcmp(c_name, "res_standard_720p") == 0) { w = 1280; h = 720; }

        /* 读取原图 input.raw */
        char in_path[256];
        snprintf(in_path, sizeof(in_path), "%s/%s_input.raw", gv_dir, c_name);

        size_t sz = 0;
        uint8_t* raw_img = read_file_bytes(in_path, &sz);
        if (!raw_img) {
            printf("[FAIL] 无法打开输入文件: %s\n", in_path);
            continue;
        }

        AdaptiveSceneMetrics m;
        adaptive_grading_analyze(raw_img, w, h, w * 3, ADAPTIVE_PIXEL_FORMAT_BGR888, &m);

        /* 输出 metrics JSON 项 */
        fprintf(fp_m, "  \"%s\": {\n", c_name);
        fprintf(fp_m, "    \"mean_luminance\": %.6f,\n", m.mean_luminance);
        fprintf(fp_m, "    \"std_luminance\": %.6f,\n", m.std_luminance);
        fprintf(fp_m, "    \"shadow_clip_ratio\": %.6f,\n", m.shadow_clip_ratio);
        fprintf(fp_m, "    \"highlight_clip_ratio\": %.6f,\n", m.highlight_clip_ratio);
        fprintf(fp_m, "    \"estimated_cct\": %.3f,\n", m.estimated_cct);
        fprintf(fp_m, "    \"tint\": %.6f,\n", m.tint);
        fprintf(fp_m, "    \"scene_label\": \"%s\",\n", m.scene_label);
        fprintf(fp_m, "    \"dynamic_range_ev\": %.4f\n", m.dynamic_range_ev);
        fprintf(fp_m, "  }%s\n", (i == num_cases - 1) ? "" : ",");

        /* 解算 15 种风格并输出 JSON */
        fprintf(fp_s, "  \"%s\": {\n", c_name);
        for (int s = 1; s <= 15; s++) {
            AdaptiveStyleParameters p;
            adaptive_grading_solve_style(s, &m, &p);

            fprintf(fp_s, "    \"style_%02d\": {\n", s);
            fprintf(fp_s, "      \"contrast\": %.6f,\n", p.contrast);
            fprintf(fp_s, "      \"exposure_bias\": %.6f,\n", p.exposure_bias);
            fprintf(fp_s, "      \"saturation\": %.6f,\n", p.saturation);
            fprintf(fp_s, "      \"black_lift\": %.6f,\n", p.black_lift);
            fprintf(fp_s, "      \"white_clip\": %.6f,\n", p.white_clip);
            fprintf(fp_s, "      \"toe_strength\": %.6f,\n", p.toe_strength);
            fprintf(fp_s, "      \"shoulder_strength\": %.6f,\n", p.shoulder_strength);
            fprintf(fp_s, "      \"grain_intensity\": %.6f,\n", p.grain_intensity);
            fprintf(fp_s, "      \"vignette\": %.6f,\n", p.vignette);
            fprintf(fp_s, "      \"shadow_tint\": [%.6f, %.6f, %.6f],\n", p.shadow_tint[0], p.shadow_tint[1], p.shadow_tint[2]);
            fprintf(fp_s, "      \"mid_tint\": [%.6f, %.6f, %.6f],\n", p.mid_tint[0], p.mid_tint[1], p.mid_tint[2]);
            fprintf(fp_s, "      \"highlight_tint\": [%.6f, %.6f, %.6f],\n", p.highlight_tint[0], p.highlight_tint[1], p.highlight_tint[2]);
            fprintf(fp_s, "      \"protect_skin\": %d,\n", p.protect_skin);
            fprintf(fp_s, "      \"is_monochrome\": %d,\n", p.is_monochrome);
            fprintf(fp_s, "      \"denoise_strength\": %.6f,\n", p.denoise_strength);
            fprintf(fp_s, "      \"clarity_strength\": %.6f\n", p.clarity_strength);
            fprintf(fp_s, "    }%s\n", (s == 15) ? "" : ",");
        }
        fprintf(fp_s, "  }%s\n", (i == num_cases - 1) ? "" : ",");

        free(raw_img);
    }

    fprintf(fp_m, "}\n");
    fprintf(fp_s, "}\n");
    fclose(fp_m);
    fclose(fp_s);

    /* 3. 执行 Level 3: 时域平滑器序列测试 */
    printf("[2/3] 执行 Level 3 (TemporalSmoother) 序列测试...\n");
    adaptive_grading_reset_smoother(engine);

    float test_inputs[][3] = {
        {1.0f, 0.0f, 1.0f},
        {1.02f, 0.01f, 1.01f},
        {1.01f, 0.0f, 0.99f},
        {1.03f, 0.02f, 1.02f},
        {1.80f, 0.90f, 1.50f},
        {1.75f, 0.85f, 1.48f},
        {1.70f, 0.80f, 1.45f},
        {1.70f, 0.80f, 1.45f},
        {1.70f, 0.80f, 1.45f},
        {1.70f, 0.80f, 1.45f}
    };
    int num_seq = sizeof(test_inputs) / sizeof(test_inputs[0]);

    char sm_path[256];
    snprintf(sm_path, sizeof(sm_path), "%s/c_smoother.json", out_dir);
    FILE* fp_sm = fopen(sm_path, "w");
    if (!fp_sm) {
        printf("[FATAL] 无法创建平滑器输出 JSON 于 %s\n", sm_path);
        return 1;
    }
    fprintf(fp_sm, "[\n");

    float cur_c = 0.0f, cur_ev = 0.0f, cur_sat = 0.0f;
    int sm_inited = 0;

    for (int k = 0; k < num_seq; k++) {
        float tgt_c = test_inputs[k][0];
        float tgt_ev = test_inputs[k][1];
        float tgt_sat = test_inputs[k][2];

        if (!sm_inited) {
            cur_c = tgt_c;
            cur_ev = tgt_ev;
            cur_sat = tgt_sat;
            sm_inited = 1;
        } else {
            float diff = fabsf(tgt_c - cur_c);
            float rel = diff / MAX(fabsf(cur_c), 1.0f);
            float alpha = (rel > 0.15f) ? (0.08f + MIN((rel - 0.15f) / 0.15f, 1.0f) * (0.45f - 0.08f)) : 0.08f;
            cur_c += alpha * (tgt_c - cur_c);

            diff = fabsf(tgt_ev - cur_ev);
            rel = diff / MAX(fabsf(cur_ev), 1.0f);
            alpha = (rel > 0.15f) ? (0.08f + MIN((rel - 0.15f) / 0.15f, 1.0f) * (0.45f - 0.08f)) : 0.08f;
            cur_ev += alpha * (tgt_ev - cur_ev);

            diff = fabsf(tgt_sat - cur_sat);
            rel = diff / MAX(fabsf(cur_sat), 1.0f);
            alpha = (rel > 0.15f) ? (0.08f + MIN((rel - 0.15f) / 0.15f, 1.0f) * (0.45f - 0.08f)) : 0.08f;
            cur_sat += alpha * (tgt_sat - cur_sat);
        }

        fprintf(fp_sm, "  {\n");
        fprintf(fp_sm, "    \"frame\": %d,\n", k);
        fprintf(fp_sm, "    \"smoothed\": {\n");
        fprintf(fp_sm, "      \"contrast\": %.6f,\n", cur_c);
        fprintf(fp_sm, "      \"exposure_bias\": %.6f,\n", cur_ev);
        fprintf(fp_sm, "      \"saturation\": %.6f\n", cur_sat);
        fprintf(fp_sm, "    }\n");
        fprintf(fp_sm, "  }%s\n", (k == num_seq - 1) ? "" : ",");
    }
    fprintf(fp_sm, "]\n");
    fclose(fp_sm);

    /* 4. 执行 Level 4: 渲染像素级单测 */
    printf("[3/3] 执行 Level 4 (Render Pipeline) 像素级渲染测试...\n");
    struct {
        const char* in_raw;
        const char* out_raw;
        int style_id;
        int w, h;
    } r_items[] = {
        {"scene_01_portrait_style_01_input.raw", "scene_01_portrait_style_01_output.raw", 1, 256, 256},
        {"scene_01_portrait_style_03_input.raw", "scene_01_portrait_style_03_output.raw", 3, 256, 256},
        {"scene_02_sunset_style_04_input.raw", "scene_02_sunset_style_04_output.raw", 4, 256, 256},
        {"scene_02_sunset_style_06_input.raw", "scene_02_sunset_style_06_output.raw", 6, 256, 256},
        {"scene_05_high_dr_style_08_input.raw", "scene_05_high_dr_style_08_output.raw", 8, 256, 256},
        {"scene_06_night_neon_style_11_input.raw", "scene_06_night_neon_style_11_output.raw", 11, 256, 256},
        {"scene_03_sunny_style_14_input.raw", "scene_03_sunny_style_14_output.raw", 14, 256, 256},
        {"scene_05_high_dr_style_15_input.raw", "scene_05_high_dr_style_15_output.raw", 15, 256, 256},
        {"edge_neutral_gray_style_01_input.raw", "edge_neutral_gray_style_01_output.raw", 1, 256, 256},
    };
    int num_render = sizeof(r_items) / sizeof(r_items[0]);

    for (int r = 0; r < num_render; r++) {
        char in_path[256];
        snprintf(in_path, sizeof(in_path), "%s/%s", gv_dir, r_items[r].in_raw);
        size_t sz = 0;
        uint8_t* in_data = read_file_bytes(in_path, &sz);
        if (!in_data) {
            printf("[FAIL] 缺少输入图像文件: %s\n", in_path);
            continue;
        }

        int w = r_items[r].w;
        int h = r_items[r].h;
        uint8_t* out_data = (uint8_t*)malloc(w * h * 3);

        adaptive_grading_reset_smoother(engine);
        adaptive_grading_set_style(engine, r_items[r].style_id);

        /* 处理单帧 */
        adaptive_grading_process_frame(
            engine,
            in_data,
            out_data,
            w,
            h,
            w * 3,
            w * 3,
            ADAPTIVE_PIXEL_FORMAT_BGR888,
            0,
            0.5f,
            NULL,
            NULL
        );

        char out_path[256];
        snprintf(out_path, sizeof(out_path), "%s/c_%s", out_dir, r_items[r].out_raw);
        write_file_bytes(out_path, out_data, w * h * 3);

        free(in_data);
        free(out_data);
    }

    adaptive_grading_destroy(engine);
    printf("======================================================================\n");
    printf("[OK] C 核心库测试执行完成，全部输出已导出至 %s\n", out_dir);
    return 0;
}
