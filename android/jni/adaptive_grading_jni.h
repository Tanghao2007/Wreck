/**
 * @file adaptive_grading_jni.h
 * @brief Android JNI 桥接层头文件
 * @details 将 C 语言核心调色引擎与 Android Java/Kotlin 运行环境无缝连接，
 *          支持 DirectByteBuffer 零拷贝直接内存传递与多线程安全交互。
 */

#ifndef ADAPTIVE_GRADING_JNI_H
#define ADAPTIVE_GRADING_JNI_H

#include "jni.h"
#include "adaptive_grading.h"
#include "adaptive_logger.h"
#include "adaptive_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 桥接包装上下文 (包含调色引擎与高频日志器) */
typedef struct {
    AdaptiveGradingEngine* engine;
    AdaptiveRingLogger*    logger;
    uint32_t               frame_counter;
} AdaptiveJniContext;

#ifdef __cplusplus
}
#endif

#endif /* ADAPTIVE_GRADING_JNI_H */

