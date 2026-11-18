/**
 * @file benchmark_c_core.c
 * @brief C 核心库在真实设备端的深度微基准与性能画像测试
 */

#include "adaptive_grading.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static double get_time_ms(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

static void run_style_benchmark(
    AdaptiveGradingEngine* engine,
    int style_id,
    const char* style_desc,
    int w, int h, int num_frames
) {
    int bpp = 4; /* RGBA 硬件格式 */
    uint8_t* in_frame = (uint8_t*)malloc(w * h * bpp);
    uint8_t* out_frame = (uint8_t*)malloc(w * h * bpp);

    for (int i = 0; i < w * h * bpp; i++) {
        in_frame[i] = (uint8_t)((i * 37 + (i >> 6)) & 0xFF);
    }

    adaptive_grading_set_style(engine, style_id);

    /* 预热 5 帧 */
    for (int i = 0; i < 5; i++) {
        adaptive_grading_process_frame(
            engine, in_frame, out_frame, w, h, w * bpp, w * bpp,
            ADAPTIVE_PIXEL_FORMAT_RGBA8888, 0, 0.5f, NULL, NULL
        );
    }

    double total_time = 0.0;
    for (int i = 0; i < num_frames; i++) {
        double t0 = get_time_ms();
        adaptive_grading_process_frame(
            engine, in_frame, out_frame, w, h, w * bpp, w * bpp,
            ADAPTIVE_PIXEL_FORMAT_RGBA8888, 0, 0.5f, NULL, NULL
        );
        double t1 = get_time_ms();
        total_time += (t1 - t0);
    }

    double avg_ms = total_time / num_frames;
    double fps = 1000.0 / avg_ms;
    printf("  [风格 %02d | %s] 单帧耗时: %6.2f ms | 帧率: %5.1f FPS\n",
           style_id, style_desc, avg_ms, fps);

    free(in_frame);
    free(out_frame);
}

int main(void) {
    printf("======================================================================\n");
    printf("  真实移动端 (高通骁龙 8 至尊版) 调色内核 15 款电影风格深度基准测试\n");
    printf("======================================================================\n");

    AdaptiveGradingEngine* engine = adaptive_grading_create(1);
    if (!engine) return 1;

    struct {
        int id;
        const char* name;
    } test_styles[] = {
        {1, "01. 自然人像 (含肤色保护+清晰度)"},
        {2, "02. 日系清新 (含肤色保护+清晰度)"},
        {3, "03. 韩系柔光 (含肤色保护+柔焦)"},
        {4, "04. 胶片复古 (纯Master LUT+颗粒)"},
        {5, "05. 富士经典 (纯Master LUT+颗粒)"},
        {6, "06. 柯达暖金 (纯Master LUT+暗角)"},
        {7, "07. 电影冷青 (纯Master LUT+高对比)"},
        {8, "08. 赛博朋克 (纯Master LUT+双色轮)"},
        {9, "09. 暮色余晖 (纯Master LUT+暖调)"},
        {10, "10. 暗调情绪 (纯Master LUT+暗部提升)"},
        {11, "11. 经典黑白 (Filmic S曲线)"},
        {12, "12. 高对比黑白 (硬朗黑白曲线)"},
        {13, "13. 柔和黑白 (微反差单色)"},
        {14, "14. 绿意森林 (纯Master LUT+饱和微调)"},
        {15, "15. 苍穹晴空 (纯Master LUT+天空蓝强化)"}
    };

    printf("\n--- [多线程扩展性测试] 720p 胶片复古风格 线程数扩展对比 ---\n");
    int thread_counts[] = {1, 2, 4, 6};
    for (int t = 0; t < 4; t++) {
        adaptive_grading_set_thread_count(engine, thread_counts[t]);
        char desc[64];
        snprintf(desc, sizeof(desc), "04. 胶片复古 (%d 线程并发)", thread_counts[t]);
        run_style_benchmark(engine, 4, desc, 1280, 720, 30);
    }

    /* 使用 4 线程进行 15 款风格全景评测 */
    adaptive_grading_set_thread_count(engine, 4);
    printf("\n--- 分辨率: 1280x720 (720p 4线程实时相机预览) ---\n");
    for (int i = 0; i < 15; i++) {
        run_style_benchmark(engine, test_styles[i].id, test_styles[i].name, 1280, 720, 30);
    }

    printf("\n--- 分辨率: 1920x1080 (1080p 全高清录像流 4线程) ---\n");
    run_style_benchmark(engine, 4, "04. 胶片复古 (1080p 4线程)", 1920, 1080, 20);
    run_style_benchmark(engine, 11, "11. 经典黑白 (1080p 4线程)", 1920, 1080, 20);
    run_style_benchmark(engine, 1, "01. 自然人像 (1080p 4线程)", 1920, 1080, 20);

    /* 6 线程极限性能评测 */
    adaptive_grading_set_thread_count(engine, 6);
    printf("\n--- 6 线程全大核并发极限性能 ---\n");
    run_style_benchmark(engine, 4, "04. 胶片复古 (720p 6线程极限)", 1280, 720, 30);
    run_style_benchmark(engine, 4, "04. 胶片复古 (1080p 6线程极限)", 1920, 1080, 20);

    /* 原生相机 NV21 直出测试 */
    printf("\n--- [相机原生流直测] Camera2/CameraX NV21 -> RGBA 4线程直出 ---\n");
    int cam_w = 1280, cam_h = 720;
    uint8_t* y_plane = (uint8_t*)malloc(cam_w * cam_h);
    uint8_t* uv_plane = (uint8_t*)malloc(cam_w * cam_h / 2);
    uint8_t* dst_rgba = (uint8_t*)malloc(cam_w * cam_h * 4);
    memset(y_plane, 128, cam_w * cam_h);
    memset(uv_plane, 128, cam_w * cam_h / 2);
    adaptive_grading_set_thread_count(engine, 4);
    adaptive_grading_set_style(engine, 4);

    double yuv_total = 0.0;
    for (int f = 0; f < 30; f++) {
        double t0 = get_time_ms();
        adaptive_grading_process_yuv420(
            engine, y_plane, uv_plane, dst_rgba, cam_w, cam_h,
            cam_w, cam_w, cam_w * 4, 1, 0, 0.5f, NULL, NULL
        );
        double t1 = get_time_ms();
        yuv_total += (t1 - t0);
    }
    double yuv_avg = yuv_total / 30.0;
    printf("  [Camera NV21 硬件直出流] 720p 4线程平均耗时: %6.2f ms | 帧率: %5.1f FPS\n",
           yuv_avg, 1000.0 / yuv_avg);

    free(y_plane);
    free(uv_plane);
    free(dst_rgba);

    adaptive_grading_destroy(engine);
    printf("======================================================================\n");
    return 0;
}
