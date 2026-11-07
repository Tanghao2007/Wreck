/**
 * @file test_app_ui_pipeline.c
 * @brief App/UI 交互层、相机采集流、生命周期与状态管理全链路端到端集成测试套件
 */

#include "adaptive_grading.h"
#include "adaptive_logger.h"
#include "adaptive_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(d) _mkdir(d)
static double get_time_ms(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <time.h>
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_tests_passed++; \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
        g_tests_failed++; \
    } \
} while (0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) == (b)) { \
        printf("  [PASS] %s\n", msg); \
        g_tests_passed++; \
    } else { \
        printf("  [FAIL] %s: %lld != %lld (Line %d)\n", msg, (long long)(a), (long long)(b), __LINE__); \
        g_tests_failed++; \
    } \
} while (0)

/* ========================================================================== */
/* 测试用例 1: 相机硬件采集流 (Camera2 / CameraX YUV420 -> RGBA) 集成测试    */
/* ========================================================================== */
static void test_camera_hardware_pipeline(AdaptiveGradingEngine* engine) {
    printf("\n--- [测试组 1] 相机硬件采集流 (Camera2/CameraX YUV420) 集成测试 ---\n");
    int w = 640, h = 360;
    int y_size = w * h;
    int uv_size = w * h / 2;
    int rgba_size = w * h * 4;

    uint8_t* y_plane = (uint8_t*)malloc(y_size);
    uint8_t* uv_plane = (uint8_t*)malloc(uv_size);
    uint8_t* dst_rgba = (uint8_t*)malloc(rgba_size);

    /* 填充模拟室内人像肤色 YUV 信号 (Y=160, U=115, V=150) */
    memset(y_plane, 160, y_size);
    for (int i = 0; i < uv_size; i += 2) {
        uv_plane[i + 0] = 150; /* V */
        uv_plane[i + 1] = 115; /* U */
    }

    adaptive_grading_set_style(engine, 1); /* 自然人像 */

    /* 1.1 NV21 流水线处理测试 */
    AdaptiveSceneMetrics metrics;
    AdaptiveStyleParameters params;
    AdaptiveResult res = adaptive_grading_process_yuv420(
        engine, y_plane, uv_plane, dst_rgba, w, h, w, w, w * 4,
        1, /* is_nv21 */
        0, /* split_mode off */
        0.5f,
        &metrics,
        &params
    );
    ASSERT_EQ(res, ADAPTIVE_OK, "Camera NV21 原生硬件流处理返回 ADAPTIVE_OK");
    ASSERT_TRUE(metrics.mean_luminance > 0.0f, "场景平均亮度有效提取");
    ASSERT_TRUE(dst_rgba[3] == 255, "Alpha 通道正确写入 255");

    /* 1.2 NV12 流水线处理测试 (UV 顺序交换) */
    for (int i = 0; i < uv_size; i += 2) {
        uv_plane[i + 0] = 115; /* U */
        uv_plane[i + 1] = 150; /* V */
    }
    res = adaptive_grading_process_yuv420(
        engine, y_plane, uv_plane, dst_rgba, w, h, w, w, w * 4,
        0, /* is_nv21 = 0 (NV12) */
        0, 0.5f, &metrics, &params
    );
    ASSERT_EQ(res, ADAPTIVE_OK, "Camera NV12 原生硬件流处理返回 ADAPTIVE_OK");

    free(y_plane);
    free(uv_plane);
    free(dst_rgba);
}

/* ========================================================================== */
/* 测试用例 2: 分屏对比无缝拖拽与交互状态机测试                              */
/* ========================================================================== */
static void test_split_screen_interactions(AdaptiveGradingEngine* engine) {
    printf("\n--- [测试组 2] 分屏对比无缝拖拽与交互状态机测试 ---\n");
    int w = 400, h = 200;
    int bpp = 4;
    uint8_t* in_frame = (uint8_t*)malloc(w * h * bpp);
    uint8_t* out_frame = (uint8_t*)malloc(w * h * bpp);

    /* 填充原图基准 (所有像素值为 80) */
    memset(in_frame, 80, w * h * bpp);
    adaptive_grading_set_style(engine, 4); /* 胶片复古 (会改变像素值) */

    /* 2.1 分屏比例 0.25 (四分之一原图，四分之三调色) */
    adaptive_grading_process_frame(
        engine, in_frame, out_frame, w, h, w * bpp, w * bpp,
        ADAPTIVE_PIXEL_FORMAT_RGBA8888, 1, 0.25f, NULL, NULL
    );
    int split_x1 = (int)(w * 0.25f);
    ASSERT_EQ(out_frame[0], 80, "比例 0.25 左侧原图完全一致");
    ASSERT_EQ(out_frame[split_x1 * 4 + 0], 255, "比例 0.25 分割线为纯白 R");
    ASSERT_EQ(out_frame[split_x1 * 4 + 1], 255, "比例 0.25 分割线为纯白 G");
    ASSERT_EQ(out_frame[split_x1 * 4 + 2], 255, "比例 0.25 分割线为纯白 B");
    ASSERT_TRUE(out_frame[(w - 1) * 4] != 80, "比例 0.25 右侧执行风格调色");

    /* 2.2 分屏比例 0.75 (四分之三原图，四分之一调色) */
    adaptive_grading_process_frame(
        engine, in_frame, out_frame, w, h, w * bpp, w * bpp,
        ADAPTIVE_PIXEL_FORMAT_RGBA8888, 1, 0.75f, NULL, NULL
    );
    int split_x2 = (int)(w * 0.75f);
    ASSERT_EQ(out_frame[0], 80, "比例 0.75 左侧原图保持一致");
    ASSERT_EQ(out_frame[(split_x2 - 10) * 4], 80, "比例 0.75 分割线左侧仍为原图");
    ASSERT_EQ(out_frame[split_x2 * 4], 255, "比例 0.75 分割线为纯白");

    /* 2.3 关闭分屏模式测试 */
    adaptive_grading_process_frame(
        engine, in_frame, out_frame, w, h, w * bpp, w * bpp,
        ADAPTIVE_PIXEL_FORMAT_RGBA8888, 0, 0.5f, NULL, NULL
    );
    ASSERT_TRUE(out_frame[0] != 80, "关闭分屏模式后整幅画面全屏应用调色");

    free(in_frame);
    free(out_frame);
}

/* ========================================================================== */
/* 测试用例 3: 动态线程控制与 CPU Oryon 核心调度测试                         */
/* ========================================================================== */
static void test_dynamic_thread_control(AdaptiveGradingEngine* engine) {
    printf("\n--- [测试组 3] 动态线程控制与 CPU 核心调度测试 ---\n");

    /* 3.1 设定 1 线程 (单核省电模式) */
    ASSERT_EQ(adaptive_grading_set_thread_count(engine, 1), ADAPTIVE_OK, "设置 1 线程返回 ADAPTIVE_OK");
    ASSERT_EQ(adaptive_grading_get_thread_count(engine), 1, "获取当前线程数为 1");

    /* 3.2 设定 4 线程 (常规相机实时模式) */
    ASSERT_EQ(adaptive_grading_set_thread_count(engine, 4), ADAPTIVE_OK, "设置 4 线程返回 ADAPTIVE_OK");
    ASSERT_EQ(adaptive_grading_get_thread_count(engine), 4, "获取当前线程数为 4");

    /* 3.3 设定 6 线程 (高刷全大核模式) */
    ASSERT_EQ(adaptive_grading_set_thread_count(engine, 6), ADAPTIVE_OK, "设置 6 线程返回 ADAPTIVE_OK");
    ASSERT_EQ(adaptive_grading_get_thread_count(engine), 6, "获取当前线程数为 6");

    /* 3.4 边界值保护 */
    adaptive_grading_set_thread_count(engine, -2);
    ASSERT_EQ(adaptive_grading_get_thread_count(engine), 1, "负线程数自动纠正为 1");

    adaptive_grading_set_thread_count(engine, 64);
    ASSERT_EQ(adaptive_grading_get_thread_count(engine), 16, "超限线程数自动限制为 16");

    adaptive_grading_set_thread_count(engine, 4);
}

/* ========================================================================== */
/* 测试用例 4: 实时日志导出与配方存储联动测试                                */
/* ========================================================================== */
static void test_logging_and_recipe_integration(AdaptiveGradingEngine* engine) {
    printf("\n--- [测试组 4] 实时日志导出与配方存储联动测试 ---\n");

    AdaptiveRingLogger* logger = adaptive_logger_create();
    ASSERT_TRUE(logger != NULL, "成功创建 Native 环形日志器");

    /* 模拟 100 帧连续相机流运行数据 */
    AdaptiveSceneMetrics m;
    memset(&m, 0, sizeof(m));
    m.mean_luminance = 0.52f;
    m.estimated_cct = 5500.0f;
    m.dynamic_range_ev = 10.5f;

    AdaptiveStyleParameters p;
    memset(&p, 0, sizeof(p));
    p.contrast = 1.35f;

    for (uint32_t i = 1; i <= 100; i++) {
        float latency = (i == 42) ? 38.5f : 16.2f; /* 模拟第 42 帧发生性能抖动 */
        adaptive_logger_record_frame(
            logger, i, latency, 1.2f, latency - 1.2f, &m, &p, 1
        );
    }

    AdaptiveLoggerSummary summary;
    adaptive_logger_get_summary(logger, &summary);
    ASSERT_EQ(summary.total_logged_frames, 100, "记录累计 100 帧日志");
    ASSERT_EQ(summary.anomaly_count, 1, "准确识别出 1 次超时异常帧");

    /* 导出 JSON */
    MKDIR("tests");
    MKDIR("tests/c_outputs");
    const char* dump_file = "tests/c_outputs/app_ui_trace_dump.json";
    AdaptiveResult res = adaptive_logger_dump_json(logger, dump_file);
    ASSERT_EQ(res, ADAPTIVE_OK, "成功导出 Native 性能分析追踪 JSON");

    FILE* fp = fopen(dump_file, "r");
    ASSERT_TRUE(fp != NULL, "导出的追踪日志文件成功生成且可读取");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fclose(fp);
        ASSERT_TRUE(sz > 500, "导出的追踪日志文件大小大于 500 字节");
    }

    adaptive_logger_destroy(logger);
}

/* ========================================================================== */
/* 测试入口主函数                                                             */
/* ========================================================================== */
int main(void) {
    printf("======================================================================\n");
    printf("  App/UI 交互层、相机采集流、生命周期与状态管理全链路端到端集成测试套件\n");
    printf("======================================================================\n");

    AdaptiveGradingEngine* engine = adaptive_grading_create(1);
    if (!engine) {
        printf("[FATAL] 无法创建调色引擎上下文！\n");
        return 1;
    }

    test_camera_hardware_pipeline(engine);
    test_split_screen_interactions(engine);
    test_dynamic_thread_control(engine);
    test_logging_and_recipe_integration(engine);

    adaptive_grading_destroy(engine);

    printf("\n======================================================================\n");
    printf(" App/UI 集成测试结果统计: 通过 = %d 项, 失败 = %d 项\n", g_tests_passed, g_tests_failed);
    printf("======================================================================\n");

    if (g_tests_failed == 0) {
        printf(">>> 判定结论: App/UI 架构与相机交互管线 100%% 通过！<<<\n\n");
        return 0;
    } else {
        printf("[ERROR] 测试存在未通过项，请检查错误日志！\n");
        return 1;
    }
}
