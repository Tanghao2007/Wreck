"""
tests/test_styles_and_engine.py
15 类审美风格与画质增强渲染引擎集成测试
"""

import unittest
import numpy as np
from core.analyzer import SceneMetrics
from core.styles import STYLE_REGISTRY
from core.engine import GradingEngine
from core.color_math import enhance_clarity_and_denoise


class TestStylesAndEngine(unittest.TestCase):
    def setUp(self):
        self.dummy_metrics = SceneMetrics(
            mean_luminance=0.48,
            std_luminance=0.22,
            shadow_clip_ratio=0.02,
            highlight_clip_ratio=0.01,
            estimated_cct=5400.0,
            tint=0.05,
            scene_label="晴朗日光 (Sunny Daylight)",
            dynamic_range_ev=8.2,
        )

    def test_all_fifteen_styles_solve_valid_parameters(self):
        """测试 15 种审美风格均可成功自适应求解且无 NaN/Inf"""
        self.assertEqual(len(STYLE_REGISTRY), 15)
        for style_id, style_obj in STYLE_REGISTRY.items():
            params = style_obj.solve(self.dummy_metrics)
            self.assertFalse(np.isnan(params.contrast))
            self.assertFalse(np.isnan(params.exposure_bias))
            self.assertFalse(np.isnan(params.saturation))
            self.assertGreater(params.contrast, 0.5)
            self.assertGreaterEqual(params.black_lift, 0.0)
            self.assertGreaterEqual(params.denoise_strength, 0.0)
            self.assertGreaterEqual(params.clarity_strength, 0.0)

    def test_engine_process_frame_across_all_styles(self):
        """测试引擎处理全部 15 种风格无报错"""
        engine = GradingEngine()
        test_frame = np.random.randint(0, 256, (240, 320, 3), dtype=np.uint8)

        for style_id in range(1, 16):
            engine.set_style(style_id)
            out, metrics, params, fps = engine.process_frame(test_frame)
            self.assertEqual(out.shape, test_frame.shape)
            self.assertEqual(out.dtype, np.uint8)
            self.assertFalse(np.any(np.isnan(out)))

    def test_enhance_clarity_and_denoise(self):
        """测试色度去噪与自适应微反差清晰度增强算子"""
        img = np.full((100, 100, 3), 120, dtype=np.uint8)
        # 添加高频斑驳噪点
        img[::2, ::2, 0] = 140
        img[1::2, 1::2, 2] = 100
        clean = enhance_clarity_and_denoise(img, denoise_strength=0.5, clarity_strength=0.4)
        self.assertEqual(clean.shape, img.shape)
        self.assertEqual(clean.dtype, np.uint8)

    def test_engine_split_mode(self):
        """测试原图/调色分屏对比模式"""
        engine = GradingEngine()
        test_frame = np.full((100, 100, 3), 128, dtype=np.uint8)
        out, _, _, _ = engine.process_frame(test_frame, split_mode=True, split_ratio=0.5)
        self.assertEqual(out.shape, test_frame.shape)


if __name__ == "__main__":
    unittest.main()
