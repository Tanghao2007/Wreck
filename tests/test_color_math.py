"""
tests/test_color_math.py
色彩科学与数学算法单元测试
"""

import unittest
import numpy as np
from core.color_math import (
    srgb_to_linear,
    linear_to_srgb,
    build_filmic_curve_lut,
    compute_skin_mask,
    generate_film_grain,
    apply_color_balance,
)


class TestColorMath(unittest.TestCase):
    def test_srgb_linear_roundtrip(self):
        arr = np.linspace(0.0, 1.0, 50, dtype=np.float32)
        linear = srgb_to_linear(arr)
        roundtrip = linear_to_srgb(linear)
        np.testing.assert_allclose(arr, roundtrip, atol=1e-3)

    def test_build_filmic_curve_lut(self):
        # 无高光滚降时，应该达到满量程 255
        lut_full = build_filmic_curve_lut(contrast=1.2, toe_strength=0.0, shoulder_strength=0.0)
        self.assertEqual(len(lut_full), 256)
        self.assertEqual(lut_full[0], 0)
        self.assertEqual(lut_full[255], 255)

        # 启用胶片高光软滚降后，高光应该柔和压制，且全局保持单调性
        lut_soft = build_filmic_curve_lut(contrast=1.2, toe_strength=0.5, shoulder_strength=0.5)
        diff = np.diff(lut_soft.astype(np.int32))
        self.assertTrue(np.all(diff >= 0))
        self.assertLess(lut_soft[255], 255)
        self.assertGreater(lut_soft[255], 220)

    def test_compute_skin_mask(self):
        # 创建人工肤色图像 (BGR)
        skin_bgr = np.full((50, 50, 3), [130, 160, 210], dtype=np.uint8)
        mask = compute_skin_mask(skin_bgr)
        self.assertEqual(mask.shape, (50, 50))
        # 肤色区域应该有显著正响应 (>0.5)
        self.assertGreater(float(np.mean(mask)), 0.5)

        # 纯蓝色背景不应该有响应
        blue_bgr = np.full((50, 50, 3), [240, 40, 20], dtype=np.uint8)
        mask_blue = compute_skin_mask(blue_bgr)
        self.assertLess(float(np.mean(mask_blue)), 0.1)

    def test_generate_film_grain(self):
        grain = generate_film_grain(64, 64, intensity=0.05)
        self.assertEqual(grain.shape, (64, 64, 1))
        # 验证均值在 0 附近
        self.assertAlmostEqual(float(np.mean(grain)), 0.0, delta=0.02)

    def test_apply_color_balance(self):
        img_f = np.full((10, 10, 3), 0.5, dtype=np.float32)
        # 给中间调加暖 (B-, G+, R+)
        balanced = apply_color_balance(
            img_f,
            shadows=(0.0, 0.0, 0.0),
            midtones=(-0.1, 0.05, 0.1),
            highlights=(0.0, 0.0, 0.0),
        )
        self.assertLess(float(balanced[0, 0, 0]), 0.5)
        self.assertGreater(float(balanced[0, 0, 1]), 0.5)
        self.assertGreater(float(balanced[0, 0, 2]), 0.5)


if __name__ == "__main__":
    unittest.main()
