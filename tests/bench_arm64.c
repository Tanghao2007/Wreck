#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "adaptive_grading.h"

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main(void) {
    int w = 1280;
    int h = 720;
    int y_size = w * h;
    int uv_size = w * h / 2;
    int rgba_size = w * h * 4;

    uint8_t* y_plane = (uint8_t*)malloc(y_size);
    uint8_t* uv_plane = (uint8_t*)malloc(uv_size);
    uint8_t* dst_rgba = (uint8_t*)malloc(rgba_size);

    memset(y_plane, 128, y_size);
    memset(uv_plane, 128, uv_size);

    AdaptiveGradingEngine* engine = adaptive_grading_create(5); /* Style 5: 港风复古 */
    printf("Engine created. Warming up...\n");

    AdaptiveSceneMetrics metrics;
    AdaptiveStyleParameters active_params;

    /* Warmup */
    for (int i = 0; i < 3; i++) {
        adaptive_grading_process_yuv420(
            engine, y_plane, uv_plane, dst_rgba, w, h, w, w, w * 4, 1, 0, 0.5f, &metrics, &active_params
        );
    }

    /* Benchmark 10 iterations */
    double t_total = 0.0;
    for (int i = 0; i < 10; i++) {
        double t0 = get_time_ms();
        adaptive_grading_process_yuv420(
            engine, y_plane, uv_plane, dst_rgba, w, h, w, w, w * 4, 1, 0, 0.5f, &metrics, &active_params
        );
        double t1 = get_time_ms();
        t_total += (t1 - t0);
    }

    printf("Average adaptive_grading_process_yuv420 (Style 5, 1280x720): %.2f ms (%.1f FPS)\n",
           t_total / 10.0, 1000.0 / (t_total / 10.0));

    free(y_plane);
    free(uv_plane);
    free(dst_rgba);
    adaptive_grading_destroy(engine);
    return 0;
}

