"""
core/analyzer.py
智能场景与光照感知引擎
负责在低分辨率下采样流（256x256）上毫秒级提取：
- 画面平均照度、反差与动态范围（Luminance, Contrast & Dynamic Range）
- 高光与暗部裁切比率（Highlight & Shadow Clipping）
- 色温与色偏差估计（Estimated CCT & Tint）
- 场景光照类型分类（Scene Lighting Classifier）
"""

from dataclasses import dataclass
import numpy as np
import cv2


@dataclass
class SceneMetrics:
    """场景特征指标数据结构"""
    mean_luminance: float       # 平均亮度 (0.0~1.0)
    std_luminance: float        # 亮度标准差/反差 (0.0~0.5)
    shadow_clip_ratio: float    # 纯黑死黑比例 (0.0~1.0)
    highlight_clip_ratio: float # 纯白过曝比例 (0.0~1.0)
    estimated_cct: float        # 估算色温 (Kelvin, 2500K~9000K)
    tint: float                 # 绿/洋红偏置 (-1.0 洋红 ~ +1.0 偏绿)
    scene_label: str            # 场景分类标签
    dynamic_range_ev: float     # 估算动态范围跨度 (EV stops, e.g. 5.0 ~ 12.0)


class SceneAnalyzer:
    """毫秒级轻量场景光照分析器"""

    def __init__(self, target_size: tuple[int, int] = (256, 256)):
        self.target_size = target_size

    def analyze(self, frame_bgr: np.ndarray) -> SceneMetrics:
        """
        分析输入视频帧，返回高维场景光照度量。
        :param frame_bgr: 原始全分辨率图像或缩略帧
        :return: SceneMetrics 结构体
        """
        # 1. 快速下采样至 256x256
        h, w = frame_bgr.shape[:2]
        if w > self.target_size[0] or h > self.target_size[1]:
            thumb = cv2.resize(frame_bgr, self.target_size, interpolation=cv2.INTER_AREA)
        else:
            thumb = frame_bgr

        # 2. 亮度统计
        gray = cv2.cvtColor(thumb, cv2.COLOR_BGR2GRAY)
        gray_f = gray.astype(np.float32) / 255.0
        
        mean_lum = float(np.mean(gray_f))
        std_lum = float(np.std(gray_f))
        
        # 截断统计 (死黑与过曝)
        shadow_clip = float(np.mean(gray < 12))
        highlight_clip = float(np.mean(gray > 245))
        
        # 估算动态范围 (98分位数与 2分位数的比值转换为 EV)
        p98 = np.percentile(gray_f, 98)
        p02 = np.percentile(gray_f, 2)
        ratio = max(p98, 0.01) / max(p02, 0.002)
        dr_ev = float(np.clip(np.log2(ratio), 3.0, 14.0))

        # 3. 色温与白平衡估计 (结合灰度世界与高光白点聚类)
        b_mean = float(np.mean(thumb[..., 0]))
        g_mean = float(np.mean(thumb[..., 1]))
        r_mean = float(np.mean(thumb[..., 2]))
        
        # 重点提取非过曝的明亮像素 (70% ~ 95% 亮度区间) 作为白平衡参考
        bright_mask = (gray >= 170) & (gray <= 245)
        if np.sum(bright_mask) > 100:
            b_sample = np.mean(thumb[..., 0][bright_mask])
            g_sample = np.mean(thumb[..., 1][bright_mask])
            r_sample = np.mean(thumb[..., 2][bright_mask])
            # 与全图均值加权融合 (60% 白点采样 + 40% 灰度世界)
            b_ref = 0.6 * b_sample + 0.4 * b_mean
            g_ref = 0.6 * g_sample + 0.4 * g_mean
            r_ref = 0.6 * r_sample + 0.4 * r_mean
        else:
            b_ref, g_ref, r_ref = b_mean, g_mean, r_mean

        # 计算 R/B 比率映射到大致色温 CCT (开尔文)
        # R/B > 1.2 为暖光 (3000K-4500K), R/B ~ 1.0 为中性 (5500K), R/B < 0.8 为冷调 (6500K-8500K)
        rb_ratio = (r_ref + 1e-4) / (b_ref + 1e-4)
        
        # 经验 CCT 拟合公式
        # rb_ratio = 1.0 -> 5500K
        # rb_ratio = 1.5 -> 3200K
        # rb_ratio = 0.7 -> 7500K
        estimated_cct = 5500.0 / (rb_ratio ** 0.85)
        estimated_cct = float(np.clip(estimated_cct, 2400.0, 9500.0))
        
        # 计算色偏 (Tint: 绿色比重偏离红蓝中值的程度)
        expected_g = (r_ref + b_ref) * 0.5
        tint = float(np.clip((g_ref - expected_g) / max(expected_g, 1.0), -1.0, 1.0))

        # 4. 场景光照类型分类判别
        scene_label = self._classify_scene(mean_lum, std_lum, dr_ev, rb_ratio, highlight_clip)

        return SceneMetrics(
            mean_luminance=mean_lum,
            std_luminance=std_lum,
            shadow_clip_ratio=shadow_clip,
            highlight_clip_ratio=highlight_clip,
            estimated_cct=estimated_cct,
            tint=tint,
            scene_label=scene_label,
            dynamic_range_ev=dr_ev,
        )

    def _classify_scene(
        self,
        mean_lum: float,
        std_lum: float,
        dr_ev: float,
        rb_ratio: float,
        highlight_clip: float,
    ) -> str:
        """规则启发式场景光照判别"""
        if mean_lum < 0.22:
            return "低照度夜景 (Low-Light Night)"
        elif dr_ev > 9.5 or (std_lum > 0.28 and highlight_clip > 0.05):
            return "大光比逆光 (High-DR Backlit)"
        elif rb_ratio > 1.35 and mean_lum > 0.35:
            return "暖阳/黄金时刻 (Golden Hour Warm)"
        elif mean_lum > 0.65 and std_lum < 0.18:
            return "高调平光/阴天 (Soft Overcast)"
        elif mean_lum >= 0.45 and std_lum >= 0.20:
            return "晴朗日光 (Sunny Daylight)"
        else:
            return "常规室内/漫反射 (Indoor Ambient)"
