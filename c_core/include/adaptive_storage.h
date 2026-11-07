/**
 * @file adaptive_storage.h
 * @brief 调色配方与预设序列化、导入导出存储模块 (Native 核心层)
 * @details 负责风格配方的二进制与 JSON 结构化编解码，提供版本校验与向下兼容性保障。
 */

#ifndef ADAPTIVE_STORAGE_H
#define ADAPTIVE_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "adaptive_grading.h"

#define ADAPTIVE_STORAGE_MAGIC 0x47524144 /* 'GRAD' */
#define ADAPTIVE_STORAGE_VERSION 1

/**
 * @brief 可持久化的调色预设模型结构体
 */
typedef struct {
    uint32_t magic;              /**< 魔数 0x47524144 ('GRAD') */
    uint16_t version;            /**< 格式版本号 */
    uint16_t style_id;           /**< 基础风格 ID (1 ~ 15) */
    char     preset_name[64];    /**< 预设名称 (UTF-8) */
    char     author[32];         /**< 创建者 */
    uint64_t created_at_sec;     /**< 创建时间戳 */
    AdaptiveStyleParameters params; /**< 完整调色着色参数集 */
    uint32_t checksum;           /**< 参数校验和 */
} AdaptivePresetData;

/* ========================================================================== */
/* 接口函数                                                                   */
/* ========================================================================== */

/**
 * @brief 将预设结构体序列化为紧凑 JSON 字符串
 * @param preset 输入预设结构体
 * @param out_buffer 输出字符缓冲区
 * @param buffer_size 缓冲区大小
 * @return 实际写入字符数，缓冲区不足返回负值
 */
int adaptive_storage_export_json(
    const AdaptivePresetData* preset,
    char* out_buffer,
    size_t buffer_size
);

/**
 * @brief 从 JSON 字符串解析反序列化预设结构体
 * @param json_str 输入 JSON 字符串
 * @param out_preset 输出预设结构体
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_storage_import_json(
    const char* json_str,
    AdaptivePresetData* out_preset
);

/**
 * @brief 导出二进制格式配方文件 (.grd)
 */
AdaptiveResult adaptive_storage_save_binary(
    const AdaptivePresetData* preset,
    const char* filepath
);

/**
 * @brief 读取二进制格式配方文件 (.grd)
 */
AdaptiveResult adaptive_storage_load_binary(
    const char* filepath,
    AdaptivePresetData* out_preset
);

#ifdef __cplusplus
}
#endif

#endif /* ADAPTIVE_STORAGE_H */

