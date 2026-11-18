"""
core/engine.py
核心实时自适应调色引擎 (Real-time Adaptive Grading Engine)
集成：
- 双流调度（下采样感知流 + 全画质着色渲染流）
- 5 大大师风格动态求解与自适应时域平滑防频闪
- 肤色保护、Filmic曲线、胶片颗粒与暗角质感合成
- 原图/调色 分屏对比与高帧率极致性能优化
"""

import time
import numpy as np
import cv2

from core.color_math import (
    build_filmic_curve_lut,
    build_master_grading_lut,
    get_saturation_matrix,
    compute_skin_mask,
    generate_film_grain,
    apply_color_balance,
    enhance_clarity_and_denoise,
)
from core.analyzer import SceneAnalyzer, SceneMetrics
from core.smoother import TemporalSmoother
from core.styles import (
    STYLE_REGISTRY,
    DEFAULT_STYLE_ID,
    BaseStyle,
    StyleParameters,
)


class GradingEngine:
    """实时智能自适应调色引擎主控"""

    def __init__(self, default_style_id: int = DEFAULT_STYLE_ID):
        self.analyzer = SceneAnalyzer(target_size=(256, 256))
        self.smoother = TemporalSmoother(base_alpha=0.08, fast_alpha=0.45, threshold=0.15)
        
        # 激活风格
        self.current_style_id = default_style_id
        self.style: BaseStyle = STYLE_REGISTRY[default_style_id]
        
        # 性能与状态记录
        self.last_time = time.perf_counter()
        self.fps = 0.0
        self.frame_count = 0
        
        # 缓存暗角纹理避免每帧重复计算
        self._cached_vignette_shape: tuple[int, int] | None = None
        self._cached_vignette_mask: np.ndarray | None = None

        # 预先生成可循环采样的银盐颗粒纹理池 (1024x1024)，消除每帧运行时生成高斯噪声的昂贵开销
        np.random.seed(42)
        self._grain_pool = np.random.normal(0.0, 1.0, (1024, 1024)).astype(np.float32)
        self._grain_frame_idx = 0

    def set_style(self, style_id: int):
        """无缝切换调色风格并平滑过渡"""
        if style_id in STYLE_REGISTRY:
            self.current_style_id = style_id
            self.style = STYLE_REGISTRY[style_id]
            # 风格切换时重置平滑器状态以快速进入新风格基调
            self.smoother.reset()

    def process_frame(
        self,
        frame_bgr: np.ndarray,
        split_mode: bool = False,
        split_ratio: float = 0.5,
    ) -> tuple[np.ndarray, SceneMetrics, StyleParameters, float]:
        """
        处理单帧画面：
        :param frame_bgr: 输入 BGR 原始帧 (uint8)
        :param split_mode: 是否开启原图与调色效果左右分屏对比
        :param split_ratio: 分屏线位置 (0.0~1.0)
        :return: (渲染输出图像, 场景分析指标, 当前平滑参数, 实时FPS)
        """
        # 帧率与计时
        now = time.perf_counter()
        dt = now - self.last_time
        self.last_time = now
        if dt > 0:
            instant_fps = 1.0 / dt
            self.fps = 0.9 * self.fps + 0.1 * instant_fps if self.fps > 0 else instant_fps

        h, w = frame_bgr.shape[:2]

        # 1. 智能感知流 (毫秒级提取指标)
        metrics = self.analyzer.analyze(frame_bgr)

        # 2. 风格解算器动态求解本帧目标参数
        raw_params = self.style.solve(metrics)

        # 3. 时域防频闪平滑滤波
        param_dict = {
            "contrast": raw_params.contrast,
            "exposure_bias": raw_params.exposure_bias,
            "saturation": raw_params.saturation,
            "black_lift": raw_params.black_lift,
            "white_clip": raw_params.white_clip,
            "toe_strength": raw_params.toe_strength,
            "shoulder_strength": raw_params.shoulder_strength,
            "grain_intensity": raw_params.grain_intensity,
            "vignette": raw_params.vignette,
            "sb": raw_params.shadow_tint[0],
            "sg": raw_params.shadow_tint[1],
            "sr": raw_params.shadow_tint[2],
            "mb": raw_params.mid_tint[0],
            "mg": raw_params.mid_tint[1],
            "mr": raw_params.mid_tint[2],
            "hb": raw_params.highlight_tint[0],
            "hg": raw_params.highlight_tint[1],
            "hr": raw_params.highlight_tint[2],
            "denoise": raw_params.denoise_strength,
            "clarity": raw_params.clarity_strength,
        }
        smoothed_dict = self.smoother.update(param_dict)

        active_params = StyleParameters(
            contrast=smoothed_dict["contrast"],
            exposure_bias=smoothed_dict["exposure_bias"],
            saturation=smoothed_dict["saturation"],
            black_lift=smoothed_dict["black_lift"],
            white_clip=smoothed_dict["white_clip"],
            toe_strength=smoothed_dict["toe_strength"],
            shoulder_strength=smoothed_dict["shoulder_strength"],
            grain_intensity=smoothed_dict["grain_intensity"],
            vignette=smoothed_dict["vignette"],
            shadow_tint=(smoothed_dict["sb"], smoothed_dict["sg"], smoothed_dict["sr"]),
            mid_tint=(smoothed_dict["mb"], smoothed_dict["mg"], smoothed_dict["mr"]),
            highlight_tint=(smoothed_dict["hb"], smoothed_dict["hg"], smoothed_dict["hr"]),
            protect_skin=raw_params.protect_skin,
            is_monochrome=raw_params.is_monochrome,
            denoise_strength=smoothed_dict["denoise"],
            clarity_strength=smoothed_dict["clarity"],
        )

        # 4. 全画质 GPU/CPU 极速渲染管线
        graded = self._render_pipeline(frame_bgr, active_params)

        # 5. 可选：左右分屏对比模式
        if split_mode:
            split_x = int(w * np.clip(split_ratio, 0.0, 1.0))
            output = graded.copy()
            # 左侧显示原图，右侧显示调色后
            output[:, :split_x] = frame_bgr[:, :split_x]
            # 绘制极细的白色分界线
            cv2.line(output, (split_x, 0), (split_x, h), (255, 255, 255), 2)
        else:
            output = graded

        return output, metrics, active_params, self.fps

    def _render_pipeline(self, frame_bgr: np.ndarray, params: StyleParameters) -> np.ndarray:
        """高性能色彩变换与质感合成渲染流水线 (深度 SIMD & LUT 优化)"""
        h, w = frame_bgr.shape[:2]

        # 0. 骁龙 8 至尊版 50MP 大底画质优化：色度保边去噪消除杂色彩斑 + 自适应高频微反差增强
        clean_frame = enhance_clarity_and_denoise(
            frame_bgr,
            denoise_strength=params.denoise_strength,
            clarity_strength=params.clarity_strength,
        )

        # A. 肤色保护预先计算 (若启用：先在 1/4 尺度感知，无肤色时直接跳过，有肤色时才上采样)
        skin_mask = None
        if params.protect_skin and not params.is_monochrome:
            small_w, small_h = max(32, w // 4), max(32, h // 4)
            small_bgr = cv2.resize(clean_frame, (small_w, small_h), interpolation=cv2.INTER_AREA)
            small_mask = compute_skin_mask(small_bgr)
            if np.max(small_mask) > 0.08:
                skin_mask = cv2.resize(small_mask, (w, h), interpolation=cv2.INTER_LINEAR)
                skin_mask = np.expand_dims(skin_mask, axis=-1)

        # B & C. 黑白或彩色管线
        if params.is_monochrome:
            b = clean_frame[..., 0].astype(np.float32)
            g = clean_frame[..., 1].astype(np.float32)
            r = clean_frame[..., 2].astype(np.float32)
            gray = 0.10 * b + 0.45 * g + 0.45 * r
            gray_u8 = np.clip(gray, 0, 255).astype(np.uint8)

            filmic_lut = build_filmic_curve_lut(
                contrast=params.contrast,
                toe_strength=params.toe_strength,
                shoulder_strength=params.shoulder_strength,
                black_lift=params.black_lift,
                white_clip=params.white_clip,
            )
            gray_curved = cv2.LUT(gray_u8, filmic_lut)
            img_graded = cv2.merge([gray_curved, gray_curved, gray_curved])
        else:
            # 单指令融合 3-通道 Master LUT (曝光补偿 + Filmic S曲线 + 暗部抬升/滚降 + 三路色轮色彩平衡)
            master_lut = build_master_grading_lut(
                contrast=params.contrast,
                exposure_bias=params.exposure_bias,
                toe_strength=params.toe_strength,
                shoulder_strength=params.shoulder_strength,
                black_lift=params.black_lift,
                white_clip=params.white_clip,
                shadow_tint=params.shadow_tint,
                mid_tint=params.mid_tint,
                highlight_tint=params.highlight_tint,
            )
            curved = cv2.LUT(clean_frame, master_lut)

            # 饱和度调整：SIMD 矩阵变换
            if abs(params.saturation - 1.0) > 0.02:
                sat_m = get_saturation_matrix(params.saturation)
                img_graded = cv2.transform(curved, sat_m)
            else:
                img_graded = curved

            # 肤色隔离保护融合 (仅在存在人像肤色时执行)
            if skin_mask is not None:
                alpha = 0.7 * skin_mask
                img_graded = np.clip(
                    img_graded.astype(np.float32) * (1.0 - alpha) +
                    clean_frame.astype(np.float32) * alpha,
                    0, 255
                ).astype(np.uint8)

        # D. 胶片物理颗粒质感合成 (利用 cv2.add/cv2.subtract 硬件饱和运算，单帧 <0.5ms)
        if params.grain_intensity > 0.005:
            self._grain_frame_idx += 1
            gh, gw = 1024, 1024
            ox = (self._grain_frame_idx * 37) % max(1, gw - min(w, gw) + 1)
            oy = (self._grain_frame_idx * 53) % max(1, gh - min(h, gh) + 1)
            tile_h, tile_w = min(h, gh), min(w, gw)
            noise_slice = self._grain_pool[oy:oy + tile_h, ox:ox + tile_w]
            
            if (tile_h, tile_w) != (h, w):
                noise_slice = cv2.resize(noise_slice, (w, h), interpolation=cv2.INTER_NEAREST)
            
            delta = noise_slice * (params.grain_intensity * 255.0)
            pos_mask = np.clip(delta, 0, 255).astype(np.uint8)
            neg_mask = np.clip(-delta, 0, 255).astype(np.uint8)
            pos_3ch = cv2.merge([pos_mask, pos_mask, pos_mask])
            neg_3ch = cv2.merge([neg_mask, neg_mask, neg_mask])
            img_graded = cv2.subtract(cv2.add(img_graded, pos_3ch), neg_3ch)

        # E. 复古光学暗角合成 (利用 cv2.multiply 进行 C++ AVX 向量化乘法)
        if params.vignette > 0.01:
            vignette_u8 = self._get_vignette_mask_u8(h, w, params.vignette)
            img_graded = cv2.multiply(img_graded, vignette_u8, scale=1.0 / 255.0)

        return img_graded

    def _get_vignette_mask_u8(self, h: int, width: int, intensity: float) -> np.ndarray:
        """获取或更新缓存的光学暗角遮罩 (uint8 格式，用于 cv2.multiply 极速计算)"""
        cache_key = (h, width, round(intensity, 2))
        if self._cached_vignette_shape != cache_key:
            y, x = np.ogrid[:h, :width]
            cx, cy = width / 2.0, h / 2.0
            max_radius = np.sqrt(cx * cx + cy * cy)
            dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / max_radius
            
            # 余弦平滑衰减
            mask = np.cos(np.clip(dist * np.pi * 0.5, 0.0, np.pi * 0.5))
            vignette_float = (1.0 - intensity) + intensity * mask
            vignette_u8 = np.clip(vignette_float * 255.0, 0, 255).astype(np.uint8)
            self._cached_vignette_mask = cv2.merge([vignette_u8, vignette_u8, vignette_u8])
            self._cached_vignette_shape = cache_key

        return self._cached_vignette_mask
