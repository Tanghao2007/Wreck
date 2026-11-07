package com.antigravity.grading

/**
 * 像素颜色格式
 */
enum class PixelFormat(val nativeId: Int) {
    BGR888(0),
    RGB888(1),
    RGBA8888(2),
    BGRA8888(3),
    NV21(4)
}

/**
 * 场景光照类型
 */
enum class SceneType(val id: Int, val label: String) {
    LOW_LIGHT_NIGHT(0, "低照度夜景 (Low-Light Night)"),
    HIGH_DR_BACKLIT(1, "大光比逆光 (High-DR Backlit)"),
    GOLDEN_HOUR_WARM(2, "暖阳/黄金时刻 (Golden Hour Warm)"),
    SOFT_OVERCAST(3, "高调平光/阴天 (Soft Overcast)"),
    SUNNY_DAYLIGHT(4, "晴朗日光 (Sunny Daylight)"),
    INDOOR_AMBIENT(5, "常规室内/漫反射 (Indoor Ambient)");

    companion object {
        fun fromId(id: Int): SceneType = values().firstOrNull { it.id == id } ?: INDOOR_AMBIENT
    }
}

/**
 * 场景感知分析指标数据模型
 */
data class SceneMetrics(
    val meanLuminance: Float,
    val stdLuminance: Float,
    val shadowClipRatio: Float,
    val highlightClipRatio: Float,
    val estimatedCct: Float,
    val tint: Float,
    val sceneType: SceneType,
    val dynamicRangeEv: Float
) {
    companion object {
        fun fromArray(arr: FloatArray): SceneMetrics {
            return SceneMetrics(
                meanLuminance = arr[0],
                stdLuminance = arr[1],
                shadowClipRatio = arr[2],
                highlightClipRatio = arr[3],
                estimatedCct = arr[4],
                tint = arr[5],
                sceneType = SceneType.fromId(arr[6].toInt()),
                dynamicRangeEv = arr[7]
            )
        }
    }
}

/**
 * 调色着色流水线参数数据模型
 */
data class StyleParameters(
    val contrast: Float,
    val exposureBias: Float,
    val saturation: Float,
    val blackLift: Float,
    val whiteClip: Float,
    val toeStrength: Float,
    val shoulderStrength: Float,
    val grainIntensity: Float,
    val vignette: Float,
    val shadowTint: FloatArray,
    val midTint: FloatArray,
    val highlightTint: FloatArray,
    val protectSkin: Boolean,
    val isMonochrome: Boolean,
    val denoiseStrength: Float,
    val clarityStrength: Float
) {
    companion object {
        fun fromArray(arr: FloatArray): StyleParameters {
            return StyleParameters(
                contrast = arr[0],
                exposureBias = arr[1],
                saturation = arr[2],
                blackLift = arr[3],
                whiteClip = arr[4],
                toeStrength = arr[5],
                shoulderStrength = arr[6],
                grainIntensity = arr[7],
                vignette = arr[8],
                shadowTint = floatArrayOf(arr[9], arr[10], arr[11]),
                midTint = floatArrayOf(0f, 0f, 0f),
                highlightTint = floatArrayOf(0f, 0f, 0f),
                protectSkin = arr[12] > 0.5f,
                isMonochrome = arr[13] > 0.5f,
                denoiseStrength = arr[14],
                clarityStrength = arr[15]
            )
        }
    }
}

/**
 * 15 款大师级艺术调色风格元数据
 */
data class StyleInfo(
    val id: Int,
    val name: String,
    val englishName: String,
    val category: String,
    val description: String,
    val colorHex: Long = 0xFF50DCFF
)

/**
 * 官方 15 款胶片审美风格全局注册目录
 */
val STYLE_CATALOG = listOf(
    StyleInfo(1, "自然人像", "Natural Authentic", "人像", "真实微反差、健康肤色感知保护与细节增强", 0xFFE0BB9B),
    StyleInfo(2, "清新日系", "Fresh Japanese Airy", "日常", "高调透亮空气感、微青冷调与柔和阴影", 0xFF9BE0E0),
    StyleInfo(3, "韩系柔光", "Korean Soft Glow", "人像", "低反差高光柔焦、牛奶肌质感与哑光暗部", 0xFFE0C4DF),
    StyleInfo(4, "胶片复古", "Vintage Film", "胶片", "经典柯达银盐物理颗粒、暖调微反差与金黄高光", 0xFFDFB36D),
    StyleInfo(5, "港风复古", "Hong Kong Vintage 90s", "复古", "浓郁红绿互补对比、90年代胶片厚重色彩", 0xFFD8785E),
    StyleInfo(6, "电影冷青", "Western Cinematic", "电影", "青橙(Teal & Orange)好莱坞色调、深邃阴影", 0xFF45A29E),
    StyleInfo(7, "暗调情绪", "Moody Dark / Noir", "情绪", "暗黑剪影质感、深邃暗角与高质感哑光黑", 0xFF4A4E69),
    StyleInfo(8, "经典黑白", "Classic Monochromatic", "黑白", "全动态范围细腻单色、丰富灰阶与银盐颗粒", 0xFFCCCCCC),
    StyleInfo(9, "高饱和鲜艳", "Vibrant Pop", "风光", "高动态反差增强、色彩通透浓郁不溢出", 0xFFFF6B6B),
    StyleInfo(10, "莫兰迪灰调", "Morandi Muted Gray", "极简", "低饱和灰调平衡、高级微反差与静谧质感", 0xFF8E9AAF),
    StyleInfo(11, "赛博朋克", "Cyberpunk Neon", "夜景", "洋红与青蓝霓虹撞色、强化暗部与高光色轮", 0xFFF72585),
    StyleInfo(12, "国风古风", "Traditional Chinese", "古风", "黛绿朱红水墨质感、优雅低饱和中间调", 0xFF9E2A2B),
    StyleInfo(13, "法式浪漫", "French Romantic", "浪漫", "暖金午后阳光滤镜、柔和米白高光沉淀", 0xFFE9C46A),
    StyleInfo(14, "商业时尚", "Commercial Editorial", "商业", "精准色准还原、杂志大片微反差细节解析", 0xFF2A9D8F),
    StyleInfo(15, "风光HDR", "Landscape HDR Clarity", "风光", "超高动态范围细节保真、蓝天绿植自然透彻", 0xFF264653)
)

