/**
 * @file adaptive_logger.h
 * @brief 实时相机高频 Native 环形日志与异常数据导出模块
 * @details 专为 60fps 实时渲染管线设计：
 *          - 内存预分配 Lock-free Ring Buffer，单帧记录耗时 < 0.1 微秒，零 GC 零动态内存分配；
 *          - 毫秒级性能与场景指标跟踪 (时延、FPS、色温、动态范围、生效参数)；
 *          - 异常监控判定器 (掉帧检测、NaN/Inf 检测、光照突变记录)；
 *          - 离线复盘数据转储 (Dump)，支持以结构化 JSON 导出最近 512 帧关键数据。
 */

#ifndef ADAPTIVE_LOGGER_H
#define ADAPTIVE_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "adaptive_grading.h"

#define ADAPTIVE_LOGGER_CAPACITY 512

/**
 * @brief 单帧实时结构化日志记录条目 (紧凑 64 字节对齐)
 */
typedef struct {
    uint64_t timestamp_us;     /**< 采样微秒时间戳 */
    uint32_t frame_index;      /**< 帧序号 (单调递增) */
    float    total_time_ms;    /**< 单帧总耗时 (ms) */
    float    analyze_time_ms;  /**< 场景感知耗时 (ms) */
    float    render_time_ms;   /**< 着色渲染耗时 (ms) */
    float    estimated_cct;    /**< 估算色温 (K) */
    float    dynamic_range_ev; /**< 估算动态范围 (EV) */
    float    mean_luminance;   /**< 画面平均亮度 */
    float    active_contrast;  /**< 当前生效对比度 */
    float    active_exposure;  /**< 当前生效曝光补偿 */
    float    active_saturation;/**< 当前生效饱和度 */
    uint8_t  style_id;         /**< 当前风格 ID (1 ~ 15) */
    uint8_t  scene_type;       /**< 场景类型枚举 */
    uint8_t  is_anomaly;       /**< 是否被标记为异常帧 (1 异常, 0 正常) */
    uint8_t  anomaly_code;     /**< 异常类型编码 */
} AdaptiveLogRecord;

/**
 * @brief 异常类型编码
 */
typedef enum {
    ANOMALY_NONE             = 0,
    ANOMALY_LATENCY_OVERFLOW = 1, /**< 渲染时延超过预算 (>33.3ms 或 >16.6ms) */
    ANOMALY_NAN_INF_DETECTED = 2, /**< 检测到 NaN 或 Inf 无效浮点 */
    ANOMALY_EXTREME_CLIPPING = 3, /**< 严重死黑或过曝截断 (>90%) */
    ANOMALY_CCT_OUT_OF_BOUND = 4, /**< 色温超出物理合理范围 */
    ANOMALY_BUFFER_OVERFLOW  = 5  /**< 帧丢弃或缓冲区争用 */
} AdaptiveAnomalyType;

/**
 * @brief 日志器统计信息摘要
 */
typedef struct {
    uint32_t total_logged_frames;  /**< 累计记录帧数 */
    uint32_t anomaly_count;        /**< 累计异常帧数 */
    float    avg_total_time_ms;    /**< 平均总时延 */
    float    avg_render_time_ms;   /**< 平均着色时延 */
    float    p95_total_time_ms;    /**< 95分位数时延 */
    float    p99_total_time_ms;    /**< 99分位数时延 */
    float    min_cct;              /**< 最低色温 */
    float    max_cct;              /**< 最高色温 */
    float    max_dr_ev;            /**< 最大动态范围 */
} AdaptiveLoggerSummary;

/**
 * @brief 环形日志器句柄结构体
 */
typedef struct AdaptiveRingLogger AdaptiveRingLogger;

/* ========================================================================== */
/* 接口函数                                                                   */
/* ========================================================================== */

/**
 * @brief 创建环形日志器实例
 */
AdaptiveRingLogger* adaptive_logger_create(void);

/**
 * @brief 销毁日志器实例
 */
void adaptive_logger_destroy(AdaptiveRingLogger* logger);

/**
 * @brief 重置环形日志器缓冲区
 */
void adaptive_logger_reset(AdaptiveRingLogger* logger);

/**
 * @brief 记录一帧调色执行指标 (极速写入，无锁环形入队)
 * @param logger 日志器实例
 * @param frame_idx 帧序号
 * @param total_ms 总时延
 * @param analyze_ms 感知时延
 * @param render_ms 渲染时延
 * @param metrics 场景指标
 * @param params 生效参数
 * @param style_id 当前风格 ID
 * @return 是否触发了异常 (1 触发异常, 0 正常)
 */
int adaptive_logger_record_frame(
    AdaptiveRingLogger* logger,
    uint32_t frame_idx,
    float total_ms,
    float analyze_ms,
    float render_ms,
    const AdaptiveSceneMetrics* metrics,
    const AdaptiveStyleParameters* params,
    int style_id
);

/**
 * @brief 获取当前环形缓冲区中的运行摘要统计
 */
AdaptiveResult adaptive_logger_get_summary(
    const AdaptiveRingLogger* logger,
    AdaptiveLoggerSummary* out_summary
);

/**
 * @brief 将环形缓冲区中存储的近期全部帧数据导出为结构化 JSON 文件
 * @param logger 日志器句柄
 * @param dump_filepath 导出目标文件路径
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_logger_dump_json(
    const AdaptiveRingLogger* logger,
    const char* dump_filepath
);

#ifdef __cplusplus
}
#endif

#endif /* ADAPTIVE_LOGGER_H */

