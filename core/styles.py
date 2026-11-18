"""
core/styles.py
15 类倾向性审美风格模型库
逐类拆解调色细节与自适应求解：
 1. 自然真实类 (Natural Authentic)
 2. 清新日系类 (Fresh Japanese Airy)
 3. 韩系柔光类 (Korean Soft Glow)
 4. 胶片复古类 (Vintage Film)
 5. 港风复古类 (Hong Kong Vintage 90s)
 6. 欧美复古/电影感类 (Western Cinematic)
 7. 暗黑情绪类 (Moody Dark / Noir)
 8. 黑白类 (Classic Monochromatic)
 9. 高饱和鲜艳类 (Vibrant Pop)
10. 低饱和莫兰迪/灰调类 (Morandi Muted Gray)
11. 赛博朋克/霓虹类 (Cyberpunk Neon)
12. 国风/古风类 (Traditional Chinese Aesthetic)
13. 法式浪漫类 (French Romantic)
14. 商业时尚类 (Commercial Editorial)
15. 风光HDR/通透类 (Landscape HDR Clarity)

每个风格均封装基准参数模型与「场景自适应动态参数解算器 (Dynamic Solver)」。
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass
import numpy as np
from core.analyzer import SceneMetrics


@dataclass
class StyleParameters:
    """传导给渲染着色管线的完整参数集"""
    contrast: float                 # S曲线反差 (0.8~2.5)
    exposure_bias: float            # 曝光补偿 EV (-2.0~+2.0)
    saturation: float               # 饱和度乘数 (0.0~2.0)
    black_lift: float               # 暗部抬升/哑光黑 (0.0~0.25)
    white_clip: float               # 高光压缩截断点 (0.8~1.0)
    toe_strength: float             # 暗部脚部软度 (0.0~1.0)
    shoulder_strength: float        # 高光滚降软度 (0.0~1.0)
    grain_intensity: float          # 胶片颗粒强度 (0.0~0.3)
    vignette: float                 # 暗角强度 (0.0~1.0)
    shadow_tint: tuple[float, float, float]    # 暗部调色 (B, G, R) 偏移量
    mid_tint: tuple[float, float, float]       # 中间调 (B, G, R) 偏移量
    highlight_tint: tuple[float, float, float] # 高光调色 (B, G, R) 偏移量
    protect_skin: bool              # 是否启用人像肤色隔离保护
    is_monochrome: bool             # 是否转换为纯黑白
    denoise_strength: float = 0.35  # 色度与暗部降噪强度 (0.0~1.0)
    clarity_strength: float = 0.35  # 5000万像素高频细节与微反差清晰度 (0.0~1.0)


class BaseStyle(ABC):
    """艺术风格抽象基类"""
    id: int
    name: str
    english_name: str
    description: str

    @abstractmethod
    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        """根据当前场景感知指标动态求解自适应渲染参数"""
        pass


# ==============================================================================
# 1. 自然真实类 (Natural Authentic)
# ==============================================================================
class NaturalAuthenticStyle(BaseStyle):
    id = 1
    name = "自然真实类"
    english_name = "Natural Authentic"
    description = "还原肉眼所见，色彩准确，低滤镜感，红润肤色与清晰细节"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 白平衡自适应补偿：偏离 5500K 时微调
        cct_err = (metrics.estimated_cct - 5500.0) / 4000.0
        b_comp = float(np.clip(-cct_err * 0.04, -0.05, 0.05))
        r_comp = float(np.clip(cct_err * 0.04, -0.05, 0.05))

        # 动态范围自适应：大光比时轻微提亮暗部、压制高光
        lift = 0.015 if metrics.dynamic_range_ev > 8.5 else 0.005
        clip = 0.98 if metrics.highlight_clip_ratio > 0.02 else 1.0

        return StyleParameters(
            contrast=1.04,
            exposure_bias=0.0,
            saturation=1.0,
            black_lift=lift,
            white_clip=clip,
            toe_strength=0.45,
            shoulder_strength=0.50,
            grain_intensity=0.0,  # 自然真实类：零颗粒，保持纯净画质
            vignette=0.0,
            shadow_tint=(b_comp, 0.0, r_comp),
            mid_tint=(0.0, 0.0, 0.0),
            highlight_tint=(0.0, 0.0, 0.0),
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.35,
            clarity_strength=0.35,  # 适度锐化，质感真实
        )


# ==============================================================================
# 2. 清新日系类 (Fresh Japanese Airy)
# ==============================================================================
class FreshJapaneseStyle(BaseStyle):
    id = 2
    name = "清新日系类"
    english_name = "Fresh Japanese Airy"
    description = "通透干净轻盈，空气感生活感，淡青奶油白，低对比褪色暗部"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 曝光偏亮营造空气感，暗部死黑时加大提亮
        lift = 0.06 + metrics.shadow_clip_ratio * 0.08
        lift = float(np.clip(lift, 0.05, 0.12))
        ev_bias = 0.30 if metrics.mean_luminance < 0.65 else 0.15

        # 偏青蓝或奶油白高光，绿色偏青
        shadow_b = 0.04
        shadow_g = 0.03
        shadow_r = -0.02
        high_b = 0.01
        high_g = 0.03
        high_r = 0.02  # 奶油白微淡青

        return StyleParameters(
            contrast=0.88,         # 低对比
            exposure_bias=ev_bias,
            saturation=0.82,       # 略低饱和
            black_lift=lift,       # 暗部提亮褪色发灰但不脏
            white_clip=0.94,       # 高光柔和
            toe_strength=0.25,
            shoulder_strength=0.85, # 轻微过曝空气感
            grain_intensity=0.005, # 极少颗粒
            vignette=0.02,
            shadow_tint=(shadow_b, shadow_g, shadow_r),
            mid_tint=(-0.01, 0.02, 0.0),
            highlight_tint=(high_b, high_g, high_r),
            protect_skin=True,     # 白皙透亮肤色保护
            is_monochrome=False,
            denoise_strength=0.40,
            clarity_strength=0.25,
        )


# ==============================================================================
# 3. 韩系柔光类 (Korean Soft Glow)
# ==============================================================================
class KoreanSoftGlowStyle(BaseStyle):
    id = 3
    name = "韩系柔光类"
    english_name = "Korean Soft Glow"
    description = "温柔精致奶油肌，低攻击性，浅米淡粉暖灰，低对比柔光高质感"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 中高调，阴影轻提，黑位不实
        ev = 0.22 if metrics.mean_luminance < 0.6 else 0.08
        
        # 奶油白、浅米、淡粉色调偏置
        shadow_tint = (-0.01, 0.01, 0.02)
        mid_tint = (-0.03, 0.01, 0.04)        # 柔和暖粉
        highlight_tint = (-0.02, 0.02, 0.04)  # 奶油高光

        return StyleParameters(
            contrast=0.86,
            exposure_bias=ev,
            saturation=0.85,       # 低饱和暖灰调
            black_lift=0.05,
            white_clip=0.92,       # 柔化高光
            toe_strength=0.30,
            shoulder_strength=0.90, # 柔焦辉光感
            grain_intensity=0.0,   # 零颗粒，追求奶油肌质感
            vignette=0.05,
            shadow_tint=shadow_tint,
            mid_tint=mid_tint,
            highlight_tint=highlight_tint,
            protect_skin=True,     # 强化白皙透粉唇色
            is_monochrome=False,
            denoise_strength=0.45, # 纯净去噪
            clarity_strength=0.20, # 柔焦平滑
        )


# ==============================================================================
# 4. 胶片复古类 (Vintage Film)
# ==============================================================================
class VintageFilmStyle(BaseStyle):
    id = 4
    name = "胶片复古类"
    english_name = "Vintage Film"
    description = "模拟胶片色彩偏移，暗部青绿灰雾，高光暖黄，怀旧颗粒与宽容度"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 暗部偏灰绿/青，高光偏暖黄/奶油
        shadow_b = 0.05
        shadow_g = 0.04
        shadow_r = -0.04
        high_b = -0.05
        high_g = 0.01
        high_r = 0.05

        return StyleParameters(
            contrast=1.14,
            exposure_bias=0.0,
            saturation=0.88,
            black_lift=0.06,       # 胶片黑位不实灰雾
            white_clip=0.96,
            toe_strength=0.60,
            shoulder_strength=0.70,
            grain_intensity=0.032, # 明显胶片颗粒
            vignette=0.22,
            shadow_tint=(shadow_b, shadow_g, shadow_r),
            mid_tint=(0.0, 0.01, 0.02),
            highlight_tint=(high_b, high_g, high_r),
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.20,
            clarity_strength=0.30,
        )


# ==============================================================================
# 5. 港风复古类 (Hong Kong Vintage 90s)
# ==============================================================================
class HongKongVintageStyle(BaseStyle):
    id = 5
    name = "港风复古类"
    english_name = "Hong Kong Vintage 90s"
    description = "90年代港片感，浓郁暧昧高对比，红唇暖黄肤色，暗部绿青与霓虹"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 高对比，暗部压深，红唇与暖黄高光
        contrast = 1.34 + (0.22 - metrics.std_luminance) * 0.5
        contrast = float(np.clip(contrast, 1.25, 1.48))

        shadow_b = 0.04
        shadow_g = 0.05
        shadow_r = -0.04
        high_b = -0.06
        high_g = 0.01
        high_r = 0.07  # 浓郁暖黄红

        return StyleParameters(
            contrast=contrast,
            exposure_bias=-0.04,
            saturation=1.18,       # 浓郁饱和
            black_lift=0.015,
            white_clip=0.98,
            toe_strength=0.80,     # 暗部压深
            shoulder_strength=0.55,
            grain_intensity=0.036,
            vignette=0.28,
            shadow_tint=(shadow_b, shadow_g, shadow_r),
            mid_tint=(-0.02, 0.02, 0.05),
            highlight_tint=(high_b, high_g, high_r),
            protect_skin=True,     # 强化蜜色暖黄与红唇血色感
            is_monochrome=False,
            denoise_strength=0.25,
            clarity_strength=0.35,
        )


# ==============================================================================
# 6. 欧美复古/电影感类 (Western Cinematic)
# ==============================================================================
class WesternCinematicStyle(BaseStyle):
    id = 6
    name = "欧美复古/电影感类"
    english_name = "Western Cinematic"
    description = "宽银幕叙事感，青橙冷暖对比，低饱和高反差，厚重暗部与健康暖肤"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 经典青橙分层：暗部青蓝，高光暖橙
        shadow_b = 0.09
        shadow_g = 0.03
        shadow_r = -0.07
        high_b = -0.08
        high_g = 0.01
        high_r = 0.07

        return StyleParameters(
            contrast=1.30,
            exposure_bias=-0.03,
            saturation=0.95,       # 有层次但不艳
            black_lift=0.018,
            white_clip=0.96,       # 高光受控
            toe_strength=0.72,
            shoulder_strength=0.68,
            grain_intensity=0.024, # 细颗粒
            vignette=0.26,
            shadow_tint=(shadow_b, shadow_g, shadow_r),
            mid_tint=(0.0, 0.01, 0.03),
            highlight_tint=(high_b, high_g, high_r),
            protect_skin=True,     # 健康暖橙肤色锁定
            is_monochrome=False,
            denoise_strength=0.30,
            clarity_strength=0.40,
        )


# ==============================================================================
# 7. 暗黑情绪类 (Moody Dark / Noir)
# ==============================================================================
class MoodyDarkStyle(BaseStyle):
    id = 7
    name = "暗黑情绪类"
    english_name = "Moody Dark / Noir"
    description = "压抑孤独神秘，低调暗沉，冷蓝青灰，低饱和高光集中，氛围优先"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 整体明显压暗，暗部偏蓝冷青
        ev = -0.35 if metrics.mean_luminance > 0.3 else -0.15
        
        shadow_b = 0.08
        shadow_g = 0.02
        shadow_r = -0.05
        mid_b = 0.03
        mid_g = -0.01
        mid_r = -0.02

        return StyleParameters(
            contrast=1.24,
            exposure_bias=ev,
            saturation=0.68,       # 低饱和冷调
            black_lift=0.01,       # 保持沉郁暗部
            white_clip=0.92,       # 控制高光
            toe_strength=0.85,
            shoulder_strength=0.45,
            grain_intensity=0.035,
            vignette=0.40,         # 强暗角聚焦中心
            shadow_tint=(shadow_b, shadow_g, shadow_r),
            mid_tint=(mid_b, mid_g, mid_r),
            highlight_tint=(-0.02, 0.0, 0.02),
            protect_skin=True,     # 偏冷苍白质感
            is_monochrome=False,
            denoise_strength=0.35,
            clarity_strength=0.30,
        )


# ==============================================================================
# 8. 黑白类 (Classic Monochromatic)
# ==============================================================================
class ClassicMonochromeStyle(BaseStyle):
    id = 8
    name = "黑白类"
    english_name = "Classic Monochromatic"
    description = "去色后靠明暗线条光影叙事，黑白灰层次丰富，纯粹影调质感"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 动态黑白反差，平光拉反差，大光比平滑
        contrast = 1.45 + (0.22 - metrics.std_luminance) * 0.6
        contrast = float(np.clip(contrast, 1.35, 1.75))

        return StyleParameters(
            contrast=contrast,
            exposure_bias=0.0,
            saturation=0.0,
            black_lift=0.0,
            white_clip=1.0,
            toe_strength=0.85,
            shoulder_strength=0.55,
            grain_intensity=0.030, # 有机银盐微粒
            vignette=0.22,
            shadow_tint=(0.0, 0.0, 0.0),
            mid_tint=(0.0, 0.0, 0.0),
            highlight_tint=(0.0, 0.0, 0.0),
            protect_skin=False,
            is_monochrome=True,
            denoise_strength=0.25,
            clarity_strength=0.45, # 黑白强调结构与线条解析力
        )


# ==============================================================================
# 9. 高饱和鲜艳类 (Vibrant Pop)
# ==============================================================================
class VibrantPopStyle(BaseStyle):
    id = 9
    name = "高饱和鲜艳类"
    english_name = "Vibrant Pop"
    description = "色彩浓烈视觉冲击强，红黄蓝绿鲜活饱满，清晰锐利商业旅行感"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 防溢出自适应：防止过度浓艳导致红黄高光溢出
        sat = 1.35 if metrics.std_luminance < 0.25 else 1.25

        return StyleParameters(
            contrast=1.20,
            exposure_bias=0.05,
            saturation=sat,        # 浓烈纯正
            black_lift=0.0,
            white_clip=0.98,       # 保留高光细节
            toe_strength=0.60,
            shoulder_strength=0.60,
            grain_intensity=0.0,   # 零颗粒，极度通透
            vignette=0.06,
            shadow_tint=(0.0, 0.0, 0.0),
            mid_tint=(0.0, 0.01, 0.02),
            highlight_tint=(-0.02, 0.01, 0.02),
            protect_skin=True,     # 防止橙脸块
            is_monochrome=False,
            denoise_strength=0.35,
            clarity_strength=0.48, # 极高锐度
        )


# ==============================================================================
# 10. 低饱和莫兰迪/灰调类 (Morandi Muted Gray)
# ==============================================================================
class MorandiMutedStyle(BaseStyle):
    id = 10
    name = "低饱和莫兰迪/灰调类"
    english_name = "Morandi Muted Gray"
    description = "色彩加灰柔和高级，灰粉灰蓝灰绿，平缓中调，色彩和谐宁静"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 平缓中调，色彩明度接近
        return StyleParameters(
            contrast=0.92,         # 中低对比
            exposure_bias=0.0,
            saturation=0.66,       # 低饱和莫兰迪色系
            black_lift=0.05,       # 黑位轻提
            white_clip=0.94,       # 高光柔化
            toe_strength=0.35,
            shoulder_strength=0.80,
            grain_intensity=0.0,   # 干净细腻
            vignette=0.08,
            shadow_tint=(0.02, 0.01, 0.01),     # 灰青灰蓝
            mid_tint=(-0.01, 0.01, 0.02),
            highlight_tint=(-0.02, 0.01, 0.03), # 燕麦暖灰
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.40,
            clarity_strength=0.25,
        )


# ==============================================================================
# 11. 赛博朋克/霓虹类 (Cyberpunk Neon)
# ==============================================================================
class CyberpunkNeonStyle(BaseStyle):
    id = 11
    name = "赛博朋克/霓虹类"
    english_name = "Cyberpunk Neon"
    description = "未来都市夜景霓虹，强色光对比，暗部蓝紫，高光洋红青光，高反差"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 夜景与霓虹光强化，暗部偏蓝紫，高光洋红/青
        shadow_b = 0.12
        shadow_g = -0.02
        shadow_r = 0.04
        high_b = -0.06
        high_g = 0.03
        high_r = 0.08

        return StyleParameters(
            contrast=1.38,
            exposure_bias=-0.12,   # 暗沉背景衬托霓虹
            saturation=1.26,
            black_lift=0.02,
            white_clip=0.98,
            toe_strength=0.75,
            shoulder_strength=0.50,
            grain_intensity=0.018,
            vignette=0.32,
            shadow_tint=(shadow_b, shadow_g, shadow_r),
            mid_tint=(0.04, -0.01, 0.04),
            highlight_tint=(high_b, high_g, high_r),
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.25,
            clarity_strength=0.45,
        )


# ==============================================================================
# 12. 国风/古风类 (Traditional Chinese Aesthetic)
# ==============================================================================
class TraditionalChineseStyle(BaseStyle):
    id = 12
    name = "国风/古风类"
    english_name = "Traditional Chinese Aesthetic"
    description = "东方含蓄水墨留白，青绿黛蓝朱红墨色，白皙暖玉肤色，绢本质感"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 暗部偏墨青，高光温润绢本微暖
        shadow_b = 0.03
        shadow_g = 0.04
        shadow_r = -0.02
        high_b = -0.03
        high_g = 0.02
        high_r = 0.04

        return StyleParameters(
            contrast=1.06,         # 柔和水墨层次
            exposure_bias=0.04,
            saturation=0.78,       # 雅致低饱和
            black_lift=0.04,       # 水墨暗部墨青
            white_clip=0.95,       # 留白柔和
            toe_strength=0.40,
            shoulder_strength=0.75,
            grain_intensity=0.012, # 绢本宣纸细微质感
            vignette=0.16,
            shadow_tint=(shadow_b, shadow_g, shadow_r),
            mid_tint=(-0.01, 0.02, 0.02),
            highlight_tint=(high_b, high_g, high_r),
            protect_skin=True,     # 暖玉白皙肤色
            is_monochrome=False,
            denoise_strength=0.35,
            clarity_strength=0.30,
        )


# ==============================================================================
# 13. 法式浪漫类 (French Romantic)
# ==============================================================================
class FrenchRomanticStyle(BaseStyle):
    id = 13
    name = "法式浪漫类"
    english_name = "French Romantic"
    description = "慵懒柔暖低对比，轻微褪色电影感，暖黄奶油淡粉，松弛生活感"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 低对比，中高调，黑位不实
        ev = 0.18 if metrics.mean_luminance < 0.65 else 0.06

        shadow_tint = (-0.02, 0.01, 0.03)  # 柔暖暗部
        mid_tint = (-0.03, 0.01, 0.04)     # 奶油暖黄
        highlight_tint = (-0.04, 0.02, 0.05) # 淡粉奶油

        return StyleParameters(
            contrast=0.93,
            exposure_bias=ev,
            saturation=0.88,
            black_lift=0.05,
            white_clip=0.93,
            toe_strength=0.32,
            shoulder_strength=0.82,
            grain_intensity=0.016, # 极细颗粒
            vignette=0.14,
            shadow_tint=shadow_tint,
            mid_tint=mid_tint,
            highlight_tint=highlight_tint,
            protect_skin=True,     # 奶油暖肤自然红润
            is_monochrome=False,
            denoise_strength=0.35,
            clarity_strength=0.25,
        )


# ==============================================================================
# 14. 商业时尚类 (Commercial Editorial)
# ==============================================================================
class CommercialEditorialStyle(BaseStyle):
    id = 14
    name = "商业时尚类"
    english_name = "Commercial Editorial"
    description = "质感纯净立体可控，光影微反差精准，肤色纯净，服装面料细节毕现"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 对比精准，高光干净，暗部有丰富纹理细节
        return StyleParameters(
            contrast=1.24,
            exposure_bias=0.0,
            saturation=1.04,       # 色彩准确纯正
            black_lift=0.01,
            white_clip=0.98,
            toe_strength=0.55,
            shoulder_strength=0.60,
            grain_intensity=0.0,   # 零颗粒，极致纯净解析力
            vignette=0.10,
            shadow_tint=(0.01, 0.01, -0.01),
            mid_tint=(0.0, 0.0, 0.0),
            highlight_tint=(0.0, 0.0, 0.01),
            protect_skin=True,     # 均匀通透有立体感
            is_monochrome=False,
            denoise_strength=0.40, # 深度消解色度彩噪
            clarity_strength=0.55, # 最大化 50MP 传感器面料与睫毛解析力
        )


# ==============================================================================
# 15. 风光HDR/通透类 (Landscape HDR Clarity)
# ==============================================================================
class LandscapeHDRStyle(BaseStyle):
    id = 15
    name = "风光HDR/通透类"
    english_name = "Landscape HDR Clarity"
    description = "大光比高动态范围，高光压回暗部提亮，天空蓝青植被鲜活，通透锐利"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        # 高动态自适应：大光比时强力压制高光死白并提亮深暗部
        toe = 0.32 if metrics.dynamic_range_ev > 8.0 else 0.45
        shoulder = 0.88 if metrics.highlight_clip_ratio > 0.02 else 0.70

        return StyleParameters(
            contrast=1.14,
            exposure_bias=0.04,
            saturation=1.20,       # 蓝天绿地鲜活通透
            black_lift=0.02,       # 消除大光比死黑
            white_clip=0.96,       # 压回天空高光
            toe_strength=toe,
            shoulder_strength=shoulder,
            grain_intensity=0.0,   # 零颗粒，远景锐利
            vignette=0.06,
            shadow_tint=(0.02, 0.02, 0.0),
            mid_tint=(0.0, 0.01, 0.01),
            highlight_tint=(-0.02, 0.01, 0.02),
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.30,
            clarity_strength=0.60, # 极限风光解析度与去雾通透感
        )


# ==============================================================================
# 16. 超风格化·正午 (Super Style: Noon) - 仿 noon.png 烈日光照与碧草晴空
# ==============================================================================
class SuperNoonStyle(BaseStyle):
    id = 16
    name = "超风格化·正午"
    english_name = "Super Style - Noon"
    description = "仿照参考照片正午强光感，纯净通透日光、明亮翠绿草坪与清澈湛蓝晴空"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        ev = 0.10 if metrics.mean_luminance < 0.50 else 0.04
        shoulder = 0.82 if metrics.highlight_clip_ratio > 0.03 else 0.65
        return StyleParameters(
            contrast=1.16,
            exposure_bias=ev,
            saturation=1.18,
            black_lift=0.012,
            white_clip=0.98,
            toe_strength=0.42,
            shoulder_strength=shoulder,
            grain_intensity=0.0,
            vignette=0.04,
            shadow_tint=(0.03, 0.01, -0.01),
            mid_tint=(0.01, 0.025, -0.01),
            highlight_tint=(0.03, 0.015, 0.0),
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.30,
            clarity_strength=0.55,
        )


# ==============================================================================
# 17. 超风格化·黄金时刻 (Super Style: Golden Hour) - 仿 golden hour.png 暖阳金芒与琥珀草坪
# ==============================================================================
class SuperGoldenHourStyle(BaseStyle):
    id = 17
    name = "超风格化·黄金时刻"
    english_name = "Super Style - Golden Hour"
    description = "仿照参考照片落日低角度金芒，浓郁琥珀暖光、金黄草坪与暖融光晕"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        cct_boost = 0.02 if metrics.estimated_cct > 5500.0 else 0.0
        return StyleParameters(
            contrast=1.18,
            exposure_bias=0.06,
            saturation=1.15,
            black_lift=0.025,
            white_clip=0.96,
            toe_strength=0.48,
            shoulder_strength=0.78,
            grain_intensity=0.008,
            vignette=0.14,
            shadow_tint=(-0.02, 0.01, 0.03 + cct_boost),
            mid_tint=(-0.045, 0.02, 0.055 + cct_boost),
            highlight_tint=(-0.085, 0.01, 0.085 + cct_boost),
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.32,
            clarity_strength=0.42,
        )


# ==============================================================================
# 18. 超风格化·落日余晖 (Super Style: Sunset) - 仿 sunset.png 暮色晚霞与紫粉天际
# ==============================================================================
class SuperSunsetStyle(BaseStyle):
    id = 18
    name = "超风格化·落日余晖"
    english_name = "Super Style - Sunset"
    description = "仿照参考照片暮色晚霞，紫红霞光天际线、深邃暖橙剪影与夕阳余晖"

    def solve(self, metrics: SceneMetrics) -> StyleParameters:
        ev = -0.04 if metrics.mean_luminance > 0.45 else 0.02
        return StyleParameters(
            contrast=1.26,
            exposure_bias=ev,
            saturation=1.25,
            black_lift=0.018,
            white_clip=0.95,
            toe_strength=0.62,
            shoulder_strength=0.65,
            grain_intensity=0.012,
            vignette=0.22,
            shadow_tint=(0.06, -0.015, 0.025),
            mid_tint=(0.015, -0.02, 0.065),
            highlight_tint=(-0.14, -0.015, 0.125),
            protect_skin=True,
            is_monochrome=False,
            denoise_strength=0.28,
            clarity_strength=0.48,
        )


# 注册全部 18 种审美风格字典 (15 款经典电影风格 + 3 款超风格化)
STYLE_REGISTRY: dict[int, BaseStyle] = {
    1: NaturalAuthenticStyle(),
    2: FreshJapaneseStyle(),
    3: KoreanSoftGlowStyle(),
    4: VintageFilmStyle(),
    5: HongKongVintageStyle(),
    6: WesternCinematicStyle(),
    7: MoodyDarkStyle(),
    8: ClassicMonochromeStyle(),
    9: VibrantPopStyle(),
    10: MorandiMutedStyle(),
    11: CyberpunkNeonStyle(),
    12: TraditionalChineseStyle(),
    13: FrenchRomanticStyle(),
    14: CommercialEditorialStyle(),
    15: LandscapeHDRStyle(),
    16: SuperNoonStyle(),
    17: SuperGoldenHourStyle(),
    18: SuperSunsetStyle(),
}

DEFAULT_STYLE_ID = 1
