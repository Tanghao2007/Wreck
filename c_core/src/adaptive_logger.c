/**
 * @file adaptive_logger.c
 * @brief Native 环形日志与异常数据导出实现
 */

#include "adaptive_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t get_time_us(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000LL) / freq.QuadPart);
}
#else
#include <time.h>
static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

struct AdaptiveRingLogger {
    AdaptiveLogRecord buffer[ADAPTIVE_LOGGER_CAPACITY];
    uint32_t head;            /**< 写入游标 */
    uint32_t count;           /**< 当前缓冲区有效条目数 (<= ADAPTIVE_LOGGER_CAPACITY) */
    uint32_t total_recorded;  /**< 累计记录帧数 */
    uint32_t anomaly_count;   /**< 累计异常帧数 */
};

AdaptiveRingLogger* adaptive_logger_create(void) {
    AdaptiveRingLogger* logger = (AdaptiveRingLogger*)calloc(1, sizeof(AdaptiveRingLogger));
    return logger;
}

void adaptive_logger_destroy(AdaptiveRingLogger* logger) {
    if (logger) free(logger);
}

void adaptive_logger_reset(AdaptiveRingLogger* logger) {
    if (logger) {
        memset(logger, 0, sizeof(AdaptiveRingLogger));
    }
}

int adaptive_logger_record_frame(
    AdaptiveRingLogger* logger,
    uint32_t frame_idx,
    float total_ms,
    float analyze_ms,
    float render_ms,
    const AdaptiveSceneMetrics* m,
    const AdaptiveStyleParameters* p,
    int style_id
) {
    if (!logger) return 0;

    AdaptiveLogRecord rec;
    memset(&rec, 0, sizeof(rec));

    rec.timestamp_us = get_time_us();
    rec.frame_index = frame_idx;
    rec.total_time_ms = total_ms;
    rec.analyze_time_ms = analyze_ms;
    rec.render_time_ms = render_ms;
    rec.style_id = (uint8_t)style_id;

    if (m) {
        rec.estimated_cct = m->estimated_cct;
        rec.dynamic_range_ev = m->dynamic_range_ev;
        rec.mean_luminance = m->mean_luminance;
        rec.scene_type = (uint8_t)m->scene_type;
    }

    if (p) {
        rec.active_contrast = p->contrast;
        rec.active_exposure = p->exposure_bias;
        rec.active_saturation = p->saturation;
    }

    /* 异常监控判定器 */
    uint8_t is_anomaly = 0;
    uint8_t anomaly_code = ANOMALY_NONE;

    if (isnan(total_ms) || isinf(total_ms) || (p && (isnan(p->contrast) || isinf(p->contrast)))) {
        is_anomaly = 1;
        anomaly_code = ANOMALY_NAN_INF_DETECTED;
    } else if (total_ms > 33.33f) {
        /* 单帧耗时超出 30fps 实时预算 (掉帧) */
        is_anomaly = 1;
        anomaly_code = ANOMALY_LATENCY_OVERFLOW;
    } else if (m && (m->estimated_cct < 2000.0f || m->estimated_cct > 10000.0f)) {
        is_anomaly = 1;
        anomaly_code = ANOMALY_CCT_OUT_OF_BOUND;
    } else if (m && (m->shadow_clip_ratio > 0.90f || m->highlight_clip_ratio > 0.90f)) {
        is_anomaly = 1;
        anomaly_code = ANOMALY_EXTREME_CLIPPING;
    }

    rec.is_anomaly = is_anomaly;
    rec.anomaly_code = anomaly_code;

    /* 环形写入 */
    logger->buffer[logger->head] = rec;
    logger->head = (logger->head + 1) % ADAPTIVE_LOGGER_CAPACITY;
    if (logger->count < ADAPTIVE_LOGGER_CAPACITY) {
        logger->count++;
    }
    logger->total_recorded++;
    if (is_anomaly) {
        logger->anomaly_count++;
    }

    return is_anomaly;
}

static int compare_floats(const void* a, const void* b) {
    float fa = *(const float*)a;
    float fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

AdaptiveResult adaptive_logger_get_summary(
    const AdaptiveRingLogger* logger,
    AdaptiveLoggerSummary* out
) {
    if (!logger || !out) return ADAPTIVE_ERR_NULL_POINTER;
    if (logger->count == 0) {
        memset(out, 0, sizeof(AdaptiveLoggerSummary));
        return ADAPTIVE_OK;
    }

    uint32_t n = logger->count;
    float* times = (float*)malloc(n * sizeof(float));
    if (!times) return ADAPTIVE_ERR_OUT_OF_MEMORY;

    double sum_total = 0.0;
    double sum_render = 0.0;
    float min_cct = 99999.0f;
    float max_cct = 0.0f;
    float max_dr = 0.0f;

    for (uint32_t i = 0; i < n; i++) {
        const AdaptiveLogRecord* r = &logger->buffer[i];
        times[i] = r->total_time_ms;
        sum_total += r->total_time_ms;
        sum_render += r->render_time_ms;

        if (r->estimated_cct > 0.0f) {
            if (r->estimated_cct < min_cct) min_cct = r->estimated_cct;
            if (r->estimated_cct > max_cct) max_cct = r->estimated_cct;
        }
        if (r->dynamic_range_ev > max_dr) max_dr = r->dynamic_range_ev;
    }

    qsort(times, n, sizeof(float), compare_floats);

    out->total_logged_frames = logger->total_recorded;
    out->anomaly_count = logger->anomaly_count;
    out->avg_total_time_ms = (float)(sum_total / n);
    out->avg_render_time_ms = (float)(sum_render / n);
    out->p95_total_time_ms = times[(int)(n * 0.95f)];
    out->p99_total_time_ms = times[(int)(n * 0.99f)];
    out->min_cct = (min_cct < 99999.0f) ? min_cct : 0.0f;
    out->max_cct = max_cct;
    out->max_dr_ev = max_dr;

    free(times);
    return ADAPTIVE_OK;
}

AdaptiveResult adaptive_logger_dump_json(
    const AdaptiveRingLogger* logger,
    const char* dump_filepath
) {
    if (!logger || !dump_filepath) return ADAPTIVE_ERR_NULL_POINTER;

    FILE* fp = fopen(dump_filepath, "w");
    if (!fp) return ADAPTIVE_ERR_INVALID_PARAM;

    AdaptiveLoggerSummary summary;
    adaptive_logger_get_summary(logger, &summary);

    fprintf(fp, "{\n");
    fprintf(fp, "  \"summary\": {\n");
    fprintf(fp, "    \"total_logged_frames\": %u,\n", summary.total_logged_frames);
    fprintf(fp, "    \"anomaly_count\": %u,\n", summary.anomaly_count);
    fprintf(fp, "    \"avg_total_time_ms\": %.3f,\n", summary.avg_total_time_ms);
    fprintf(fp, "    \"avg_render_time_ms\": %.3f,\n", summary.avg_render_time_ms);
    fprintf(fp, "    \"p95_total_time_ms\": %.3f,\n", summary.p95_total_time_ms);
    fprintf(fp, "    \"p99_total_time_ms\": %.3f,\n", summary.p99_total_time_ms);
    fprintf(fp, "    \"min_cct\": %.1f,\n", summary.min_cct);
    fprintf(fp, "    \"max_cct\": %.1f,\n", summary.max_cct);
    fprintf(fp, "    \"max_dr_ev\": %.2f\n", summary.max_dr_ev);
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"recent_frames\": [\n");

    /* 按时间顺序遍历 (从最老到最新) */
    uint32_t start_idx = (logger->count < ADAPTIVE_LOGGER_CAPACITY) ? 0 : logger->head;
    for (uint32_t i = 0; i < logger->count; i++) {
        uint32_t idx = (start_idx + i) % ADAPTIVE_LOGGER_CAPACITY;
        const AdaptiveLogRecord* r = &logger->buffer[idx];

        fprintf(fp, "    {\n");
        fprintf(fp, "      \"frame_index\": %u,\n", r->frame_index);
        fprintf(fp, "      \"timestamp_us\": %llu,\n", (unsigned long long)r->timestamp_us);
        fprintf(fp, "      \"total_time_ms\": %.3f,\n", r->total_time_ms);
        fprintf(fp, "      \"analyze_time_ms\": %.3f,\n", r->analyze_time_ms);
        fprintf(fp, "      \"render_time_ms\": %.3f,\n", r->render_time_ms);
        fprintf(fp, "      \"style_id\": %u,\n", r->style_id);
        fprintf(fp, "      \"scene_type\": %u,\n", r->scene_type);
        fprintf(fp, "      \"estimated_cct\": %.1f,\n", r->estimated_cct);
        fprintf(fp, "      \"dynamic_range_ev\": %.2f,\n", r->dynamic_range_ev);
        fprintf(fp, "      \"mean_luminance\": %.3f,\n", r->mean_luminance);
        fprintf(fp, "      \"active_contrast\": %.3f,\n", r->active_contrast);
        fprintf(fp, "      \"active_exposure\": %.3f,\n", r->active_exposure);
        fprintf(fp, "      \"active_saturation\": %.3f,\n", r->active_saturation);
        fprintf(fp, "      \"is_anomaly\": %u,\n", r->is_anomaly);
        fprintf(fp, "      \"anomaly_code\": %u\n", r->anomaly_code);
        fprintf(fp, "    }%s\n", (i == logger->count - 1) ? "" : ",");
    }

    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    fclose(fp);
    return ADAPTIVE_OK;
}

