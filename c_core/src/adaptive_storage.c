/**
 * @file adaptive_storage.c
 * @brief 调色配方与预设序列化存储模块实现
 */

#include "adaptive_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 计算参数块的简易 Fletcher32 校验和 */
static uint32_t compute_checksum(const AdaptiveStyleParameters* p) {
    const uint16_t* data = (const uint16_t*)p;
    size_t words = sizeof(AdaptiveStyleParameters) / 2;
    uint32_t sum1 = 0xffff, sum2 = 0xffff;

    while (words) {
        size_t tlen = words > 359 ? 359 : words;
        words -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    }
    sum1 = (sum1 & 0xffff) + (sum1 >> 16);
    sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    return (sum2 << 16) | sum1;
}

int adaptive_storage_export_json(
    const AdaptivePresetData* preset,
    char* buf,
    size_t buf_size
) {
    if (!preset || !buf || buf_size == 0) return -1;
    const AdaptiveStyleParameters* p = &preset->params;

    int written = snprintf(
        buf, buf_size,
        "{\n"
        "  \"format\": \"adaptive_grading_recipe\",\n"
        "  \"version\": %u,\n"
        "  \"style_id\": %u,\n"
        "  \"preset_name\": \"%s\",\n"
        "  \"author\": \"%s\",\n"
        "  \"created_at\": %llu,\n"
        "  \"parameters\": {\n"
        "    \"contrast\": %.4f,\n"
        "    \"exposure_bias\": %.4f,\n"
        "    \"saturation\": %.4f,\n"
        "    \"black_lift\": %.4f,\n"
        "    \"white_clip\": %.4f,\n"
        "    \"toe_strength\": %.4f,\n"
        "    \"shoulder_strength\": %.4f,\n"
        "    \"grain_intensity\": %.4f,\n"
        "    \"vignette\": %.4f,\n"
        "    \"shadow_tint\": [%.4f, %.4f, %.4f],\n"
        "    \"mid_tint\": [%.4f, %.4f, %.4f],\n"
        "    \"highlight_tint\": [%.4f, %.4f, %.4f],\n"
        "    \"protect_skin\": %d,\n"
        "    \"is_monochrome\": %d,\n"
        "    \"denoise_strength\": %.4f,\n"
        "    \"clarity_strength\": %.4f\n"
        "  }\n"
        "}\n",
        preset->version,
        preset->style_id,
        preset->preset_name,
        preset->author,
        (unsigned long long)preset->created_at_sec,
        p->contrast, p->exposure_bias, p->saturation,
        p->black_lift, p->white_clip, p->toe_strength, p->shoulder_strength,
        p->grain_intensity, p->vignette,
        p->shadow_tint[0], p->shadow_tint[1], p->shadow_tint[2],
        p->mid_tint[0], p->mid_tint[1], p->mid_tint[2],
        p->highlight_tint[0], p->highlight_tint[1], p->highlight_tint[2],
        p->protect_skin, p->is_monochrome,
        p->denoise_strength, p->clarity_strength
    );

    if (written < 0 || (size_t)written >= buf_size) {
        return -1;
    }
    return written;
}

static float parse_json_float(const char* json, const char* key, float def_val) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* pos = strstr(json, pattern);
    if (!pos) return def_val;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t') pos++;
    return (float)atof(pos);
}

static int parse_json_int(const char* json, const char* key, int def_val) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* pos = strstr(json, pattern);
    if (!pos) return def_val;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t') pos++;
    return atoi(pos);
}

AdaptiveResult adaptive_storage_import_json(
    const char* json_str,
    AdaptivePresetData* out
) {
    if (!json_str || !out) return ADAPTIVE_ERR_NULL_POINTER;

    memset(out, 0, sizeof(AdaptivePresetData));
    out->magic = ADAPTIVE_STORAGE_MAGIC;
    out->version = (uint16_t)parse_json_int(json_str, "version", 1);
    out->style_id = (uint16_t)parse_json_int(json_str, "style_id", 1);

    /* 提取名称 */
    const char* p_name = strstr(json_str, "\"preset_name\":");
    if (p_name) {
        const char* q1 = strchr(p_name + 14, '\"');
        if (q1) {
            const char* q2 = strchr(q1 + 1, '\"');
            if (q2 && (size_t)(q2 - q1 - 1) < sizeof(out->preset_name)) {
                strncpy(out->preset_name, q1 + 1, q2 - q1 - 1);
            }
        }
    }

    AdaptiveStyleParameters* p = &out->params;
    p->contrast = parse_json_float(json_str, "contrast", 1.0f);
    p->exposure_bias = parse_json_float(json_str, "exposure_bias", 0.0f);
    p->saturation = parse_json_float(json_str, "saturation", 1.0f);
    p->black_lift = parse_json_float(json_str, "black_lift", 0.0f);
    p->white_clip = parse_json_float(json_str, "white_clip", 1.0f);
    p->toe_strength = parse_json_float(json_str, "toe_strength", 0.5f);
    p->shoulder_strength = parse_json_float(json_str, "shoulder_strength", 0.5f);
    p->grain_intensity = parse_json_float(json_str, "grain_intensity", 0.0f);
    p->vignette = parse_json_float(json_str, "vignette", 0.0f);
    p->protect_skin = parse_json_int(json_str, "protect_skin", 1);
    p->is_monochrome = parse_json_int(json_str, "is_monochrome", 0);
    p->denoise_strength = parse_json_float(json_str, "denoise_strength", 0.35f);
    p->clarity_strength = parse_json_float(json_str, "clarity_strength", 0.35f);

    out->checksum = compute_checksum(p);
    return ADAPTIVE_OK;
}

AdaptiveResult adaptive_storage_save_binary(
    const AdaptivePresetData* preset,
    const char* filepath
) {
    if (!preset || !filepath) return ADAPTIVE_ERR_NULL_POINTER;
    FILE* fp = fopen(filepath, "wb");
    if (!fp) return ADAPTIVE_ERR_INVALID_PARAM;

    AdaptivePresetData data = *preset;
    data.magic = ADAPTIVE_STORAGE_MAGIC;
    data.checksum = compute_checksum(&data.params);

    size_t written = fwrite(&data, sizeof(AdaptivePresetData), 1, fp);
    fclose(fp);

    return (written == 1) ? ADAPTIVE_OK : ADAPTIVE_ERR_EXECUTION_FAIL;
}

AdaptiveResult adaptive_storage_load_binary(
    const char* filepath,
    AdaptivePresetData* out
) {
    if (!filepath || !out) return ADAPTIVE_ERR_NULL_POINTER;
    FILE* fp = fopen(filepath, "rb");
    if (!fp) return ADAPTIVE_ERR_INVALID_PARAM;

    AdaptivePresetData data;
    size_t read_cnt = fread(&data, sizeof(AdaptivePresetData), 1, fp);
    fclose(fp);

    if (read_cnt != 1) return ADAPTIVE_ERR_EXECUTION_FAIL;
    if (data.magic != ADAPTIVE_STORAGE_MAGIC) return ADAPTIVE_ERR_INVALID_PARAM;

    uint32_t expected_chk = compute_checksum(&data.params);
    if (data.checksum != expected_chk) {
        return ADAPTIVE_ERR_INVALID_PARAM; /* 校验和不匹配，文件损坏 */
    }

    *out = data;
    return ADAPTIVE_OK;
}

