/**
 * @file adaptive_grading.h
 * @brief 实时智能自适应调色引擎 (Real-time Intelligent Adaptive Grading Engine)
 * @details 纯 C (C99/C11) 架构，去除对 PC、Python、OpenCV 等外部重型库的依赖。
 *          专为 Android 移动平台 (ARM64 / 骁龙 8 至尊版 等旗舰平台) 高实时性、低内存占用设计。
 *          支持双流感知架构：低分辨率快速场景感知 (256x256) 与全分辨率硬件级着色渲染流水线。
 */

#ifndef ADAPTIVE_GRADING_H
#define ADAPTIVE_GRADING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ========================================================================== */
/* 常量与枚举定义                                                             */
/* ========================================================================== */

#define ADAPTIVE_GRADING_VERSION_MAJOR 1
#define ADAPTIVE_GRADING_VERSION_MINOR 0
#define ADAPTIVE_GRADING_VERSION_PATCH 0

#define ADAPTIVE_STYLE_COUNT 15
#define ADAPTIVE_ANALYZER_TARGET_SIZE 256
#define ADAPTIVE_GRAIN_POOL_SIZE 1024

/**
 * @brief 像素颜色格式枚举
 */
typedef enum {
    ADAPTIVE_PIXEL_FORMAT_BGR888  = 0,  /**< BGR 3通道 (与 OpenCV 默认一致) */
    ADAPTIVE_PIXEL_FORMAT_RGB888  = 1,  /**< RGB 3通道 */
    ADAPTIVE_PIXEL_FORMAT_RGBA8888 = 2, /**< RGBA 4通道 (Android Camera2/Surface 常用) */
    ADAPTIVE_PIXEL_FORMAT_BGRA8888 = 3, /**< BGRA 4通道 */
    ADAPTIVE_PIXEL_FORMAT_NV21     = 4  /**< YUV420sp (NV21, Android 原始相机流) */
} AdaptivePixelFormat;

/**
 * @brief 场景光照分类枚举
 */
typedef enum {
    ADAPTIVE_SCENE_LOW_LIGHT_NIGHT   = 0, /**< 低照度夜景 (Low-Light Night) */
    ADAPTIVE_SCENE_HIGH_DR_BACKLIT   = 1, /**< 大光比逆光 (High-DR Backlit) */
    ADAPTIVE_SCENE_GOLDEN_HOUR_WARM  = 2, /**< 暖阳/黄金时刻 (Golden Hour Warm) */
    ADAPTIVE_SCENE_SOFT_OVERCAST     = 3, /**< 高调平光/阴天 (Soft Overcast) */
    ADAPTIVE_SCENE_SUNNY_DAYLIGHT    = 4, /**< 晴朗日光 (Sunny Daylight) */
    ADAPTIVE_SCENE_INDOOR_AMBIENT    = 5  /**< 常规室内/漫反射 (Indoor Ambient) */
} AdaptiveSceneType;

/**
 * @brief 15 类倾向性审美风格枚举
 */
typedef enum {
    ADAPTIVE_STYLE_NATURAL_AUTHENTIC      = 1,  /**< 01. 自然真实类 */
    ADAPTIVE_STYLE_FRESH_JAPANESE         = 2,  /**< 02. 清新日系类 */
    ADAPTIVE_STYLE_KOREAN_SOFT_GLOW       = 3,  /**< 03. 韩系柔光类 */
    ADAPTIVE_STYLE_VINTAGE_FILM           = 4,  /**< 04. 胶片复古类 */
    ADAPTIVE_STYLE_HONG_KONG_VINTAGE      = 5,  /**< 05. 港风复古类 */
    ADAPTIVE_STYLE_WESTERN_CINEMATIC      = 6,  /**< 06. 欧美复古/电影感类 */
    ADAPTIVE_STYLE_MOODY_DARK             = 7,  /**< 07. 暗黑情绪类 */
    ADAPTIVE_STYLE_CLASSIC_MONOCHROME     = 8,  /**< 08. 经典黑白类 */
    ADAPTIVE_STYLE_VIBRANT_POP            = 9,  /**< 09. 高饱和鲜艳类 */
    ADAPTIVE_STYLE_MORANDI_MUTED          = 10, /**< 10. 低饱和莫兰迪/灰调类 */
    ADAPTIVE_STYLE_CYBERPUNK_NEON         = 11, /**< 11. 赛博朋克/霓虹类 */
    ADAPTIVE_STYLE_TRADITIONAL_CHINESE    = 12, /**< 12. 国风/古风类 */
    ADAPTIVE_STYLE_FRENCH_ROMANTIC        = 13, /**< 13. 法式浪漫类 */
    ADAPTIVE_STYLE_COMMERCIAL_EDITORIAL   = 14, /**< 14. 商业时尚类 */
    ADAPTIVE_STYLE_LANDSCAPE_HDR          = 15  /**< 15. 风光HDR/通透类 */
} AdaptiveStyleId;

/**
 * @brief 状态与错误码定义
 */
typedef enum {
    ADAPTIVE_OK                  = 0,
    ADAPTIVE_ERR_INVALID_PARAM   = -1,
    ADAPTIVE_ERR_OUT_OF_MEMORY   = -2,
    ADAPTIVE_ERR_UNSUPPORTED_FMT = -3,
    ADAPTIVE_ERR_NULL_POINTER    = -4,
    ADAPTIVE_ERR_EXECUTION_FAIL  = -5
} AdaptiveResult;

/* ========================================================================== */
/* 核心数据结构                                                               */
/* ========================================================================== */

/**
 * @brief 场景感知分析指标结构体
 */
typedef struct {
    float mean_luminance;       /**< 画面平均亮度 (0.0 ~ 1.0) */
    float std_luminance;        /**< 亮度标准差 / 反差 (0.0 ~ 0.5) */
    float shadow_clip_ratio;    /**< 纯黑死黑比例 (0.0 ~ 1.0) */
    float highlight_clip_ratio; /**< 纯白过曝比例 (0.0 ~ 1.0) */
    float estimated_cct;        /**< 估算色温 (Kelvin, 2400K ~ 9500K) */
    float tint;                 /**< 偏绿/偏洋红程度 (-1.0 ~ +1.0) */
    AdaptiveSceneType scene_type; /**< 场景类型枚举 */
    char scene_label[64];       /**< 场景描述标签 */
    float dynamic_range_ev;     /**< 动态范围跨度 (EV stops, 3.0 ~ 14.0) */
} AdaptiveSceneMetrics;

/**
 * @brief 调色着色流水线完整参数集
 */
typedef struct {
    float contrast;             /**< Filmic S曲线反差 (0.8 ~ 2.5) */
    float exposure_bias;        /**< 曝光补偿 EV (-2.0 ~ +2.0) */
    float saturation;           /**< 饱和度乘数 (0.0 ~ 2.0) */
    float black_lift;           /**< 暗部抬升/哑光黑 (0.0 ~ 0.25) */
    float white_clip;           /**< 高光压缩截断点 (0.8 ~ 1.0) */
    float toe_strength;         /**< 暗部脚部软度 (0.0 ~ 1.0) */
    float shoulder_strength;    /**< 高光滚降软度 (0.0 ~ 1.0) */
    float grain_intensity;      /**< 胶片颗粒强度 (0.0 ~ 0.3) */
    float vignette;             /**< 暗角强度 (0.0 ~ 1.0) */
    float shadow_tint[3];       /**< 暗部色彩偏移 [B, G, R] */
    float mid_tint[3];          /**< 中间调色彩偏移 [B, G, R] */
    float highlight_tint[3];    /**< 高光色彩偏移 [B, G, R] */
    int   protect_skin;         /**< 是否启用人像肤色隔离保护 (1 启用, 0 禁用) */
    int   is_monochrome;        /**< 是否纯黑白模式 (1 黑白, 0 彩色) */
    float denoise_strength;     /**< 色度与暗部降噪强度 (0.0 ~ 1.0) */
    float clarity_strength;     /**< 微反差与高频细节清晰度 (0.0 ~ 1.0) */
} AdaptiveStyleParameters;

/**
 * @brief 时域平滑器配置
 */
typedef struct {
    float base_alpha;           /**< 稳态平滑系数 (默认 0.08) */
    float fast_alpha;           /**< 突变跟随系数 (默认 0.45) */
    float threshold;            /**< 判定突变的相对变化阈值 (默认 0.15) */
} AdaptiveSmootherConfig;

/**
 * @brief 调色引擎不透明上下文句柄
 */
typedef struct AdaptiveGradingEngine AdaptiveGradingEngine;

/* ========================================================================== */
/* 核心接口函数声明                                                           */
/* ========================================================================== */

/**
 * @brief 创建调色引擎实例并分配相关内部工作缓冲区
 * @param default_style_id 初始艺术风格 ID (1 ~ 15)
 * @return 引擎指针，内存不足时返回 NULL
 */
AdaptiveGradingEngine* adaptive_grading_create(int default_style_id);

/**
 * @brief 销毁调色引擎实例，释放所有分配的内存
 * @param engine 引擎指针
 */
void adaptive_grading_destroy(AdaptiveGradingEngine* engine);

/**
 * @brief 切换当前激活的艺术审美风格
 * @param engine 引擎指针
 * @param style_id 目标风格 ID (1 ~ 15)
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_grading_set_style(AdaptiveGradingEngine* engine, int style_id);

/**
 * @brief 获取当前激活的风格 ID
 */
int adaptive_grading_get_style(const AdaptiveGradingEngine* engine);

/**
 * @brief 重置时域平滑器状态（在场景剧烈切换或重置取景时调用）
 */
void adaptive_grading_reset_smoother(AdaptiveGradingEngine* engine);

/**
 * @brief 配置时域平滑器参数
 */
AdaptiveResult adaptive_grading_configure_smoother(
    AdaptiveGradingEngine* engine,
    const AdaptiveSmootherConfig* config
);

/**
 * @brief 独立感知分析接口：在输入帧上毫秒级提取场景光照特征
 * @param src_data 输入图像字节指针
 * @param width 图像宽度
 * @param height 图像高度
 * @param stride 图像每行字节数 (支持内存对齐)
 * @param format 像素格式 (BGR888, RGB888, RGBA8888 等)
 * @param out_metrics 输出场景度量指标
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_grading_analyze(
    const uint8_t* src_data,
    int width,
    int height,
    int stride,
    AdaptivePixelFormat format,
    AdaptiveSceneMetrics* out_metrics
);

/**
 * @brief 独立风格解算接口：根据场景度量静态求解指定风格的目标参数集
 * @param style_id 风格 ID (1 ~ 15)
 * @param metrics 输入的场景度量
 * @param out_params 输出风格求解参数
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_grading_solve_style(
    int style_id,
    const AdaptiveSceneMetrics* metrics,
    AdaptiveStyleParameters* out_params
);

/**
 * @brief 全流程单帧自适应调色处理管线
 * @details 执行：快速下采样感知 -> 风格自适应解算 -> 时域平滑防频闪 ->
 *          50MP色度去噪与微反差清晰度 -> 3通道Master LUT融合 -> 饱和度矩阵 ->
 *          人像肤色隔离保护 -> 银盐颗粒合成 -> 光学暗角 -> 可选分屏对比。
 * @param engine 调色引擎上下文
 * @param src_data 原始输入帧缓冲区
 * @param dst_data 输出图像缓冲区 (允许与 src_data 相同实现原地就地处理)
 * @param width 宽度 (像素)
 * @param height 高度 (像素)
 * @param src_stride 输入行跨度字节数 (width * bpp 或对齐后大小)
 * @param dst_stride 输出行跨度字节数
 * @param format 颜色格式 (BGR888, RGB888, RGBA8888)
 * @param split_mode 是否开启左右分屏对比模式 (1 开启, 0 关闭)
 * @param split_ratio 分屏线比例 (0.0 ~ 1.0, 常用 0.5)
 * @param out_metrics 可选输出场景感知指标 (传 NULL 忽略)
 * @param out_active_params 可选输出当前平滑后实际生效参数 (传 NULL 忽略)
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_grading_process_frame(
    AdaptiveGradingEngine* engine,
    const uint8_t* src_data,
    uint8_t* dst_data,
    int width,
    int height,
    int src_stride,
    int dst_stride,
    AdaptivePixelFormat format,
    int split_mode,
    float split_ratio,
    AdaptiveSceneMetrics* out_metrics,
    AdaptiveStyleParameters* out_active_params
);

/**
 * @brief 仅执行全画质渲染流水线（直接根据外部指定参数渲染，用于回放或手动参数微调）
 */
AdaptiveResult adaptive_grading_render_pipeline(
    AdaptiveGradingEngine* engine,
    const uint8_t* src_data,
    uint8_t* dst_data,
    int width,
    int height,
    int src_stride,
    int dst_stride,
    AdaptivePixelFormat format,
    const AdaptiveStyleParameters* params
);

/**
 * @brief 相机原生 YUV420 (NV21 / NV12) 极速硬件流直出全流程自适应渲染管线
 * @details 专为 Android CameraX / Camera2 (ImageAnalysis ImageReader) 设计：
 *          输入硬件直出 Y/UV 连续物理平面，直接融合转色、分析、求解、多线程着色输出 RGBA。
 * @param engine 调色引擎上下文
 * @param y_plane 相机 Y 平面指针
 * @param uv_plane 相机 UV 平面指针
 * @param dst_rgba 输出 RGBA 目标帧缓冲区
 * @param width 宽度 (像素)
 * @param height 高度 (像素)
 * @param y_stride Y 平面行步长
 * @param uv_stride UV 平面行步长
 * @param dst_stride 输出 RGBA 行步长
 * @param is_nv21 1 为 NV21 (Android 默认), 0 为 NV12
 * @param split_mode 是否开启分屏对比
 * @param split_ratio 分屏比例 (0.0 ~ 1.0)
 * @param out_metrics 可选场景度量指标输出
 * @param out_active_params 可选当前生效参数输出
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_grading_process_yuv420(
    AdaptiveGradingEngine* engine,
    const uint8_t* y_plane,
    const uint8_t* uv_plane,
    uint8_t* dst_rgba,
    int width,
    int height,
    int y_stride,
    int uv_stride,
    int dst_stride,
    int is_nv21,
    int split_mode,
    float split_ratio,
    AdaptiveSceneMetrics* out_metrics,
    AdaptiveStyleParameters* out_active_params
);

/**
 * @brief 设置多线程并发渲染工作线程数
 * @param engine 引擎指针
 * @param num_threads 线程数 (1 为单线程标量/NEON，2~8 为多线程并发切片渲染，推荐 4~6)
 * @return ADAPTIVE_OK 或错误码
 */
AdaptiveResult adaptive_grading_set_thread_count(AdaptiveGradingEngine* engine, int num_threads);

/**
 * @brief 获取当前多线程并发渲染工作线程数
 */
int adaptive_grading_get_thread_count(const AdaptiveGradingEngine* engine);

/* ========================================================================== */
/* 辅助信息与元数据工具函数                                                   */
/* ========================================================================== */

/**
 * @brief 配置或加载外部胶片银盐颗粒池 (1024x1024 float32)
 */
AdaptiveResult adaptive_grading_set_grain_pool(
    AdaptiveGradingEngine* engine,
    const float* pool_data
);

/**
 * @brief 获取引擎版本字符串
 */
const char* adaptive_grading_get_version(void);

/**
 * @brief 获取指定风格的中文名称
 */
const char* adaptive_style_id_to_name(int style_id);

/**
 * @brief 获取指定风格的英文名称
 */
const char* adaptive_style_id_to_english_name(int style_id);

/**
 * @brief 获取场景分类的描述文本
 */
const char* adaptive_scene_type_to_string(AdaptiveSceneType type);

#ifdef __cplusplus
}
#endif

#endif /* ADAPTIVE_GRADING_H */
