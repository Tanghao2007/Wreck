"""
tests/test_analyzer.py
智能场景与光照感知引擎单元测试
"""

import unittest
import numpy as np
from core.analyzer import SceneAnalyzer, SceneMetrics


class TestAnalyzer(unittest.TestCase):
    def setUp(self):
        self.analyzer = SceneAnalyzer()

    def test_dark_scene_analysis(self):
        # 模拟低光暗夜场景
        dark_img = np.full((120, 160, 3), 20, dtype=np.uint8)
        metrics = self.analyzer.analyze(dark_img)
        self.assertIsInstance(metrics, SceneMetrics)
        self.assertLess(metrics.mean_luminance, 0.25)
        self.assertIn("低照度夜景", metrics.scene_label)

    def test_warm_sunset_scene(self):
        # 模拟暖光/日落场景 (R很高, B较低)
        sunset_img = np.zeros((100, 100, 3), dtype=np.uint8)
        sunset_img[..., 0] = 50   # Blue
        sunset_img[..., 1] = 130  # Green
        sunset_img[..., 2] = 230  # Red
        metrics = self.analyzer.analyze(sunset_img)
        self.assertLess(metrics.estimated_cct, 4500.0) # 暖色温
        self.assertIn("暖", metrics.scene_label)

    def test_dynamic_range_bounds(self):
        # 模拟大动态范围场景 (高光与死黑共存)
        high_dr = np.zeros((200, 200, 3), dtype=np.uint8)
        high_dr[:100, :] = 250
        high_dr[100:, :] = 1
        metrics = self.analyzer.analyze(high_dr)
        self.assertGreater(metrics.dynamic_range_ev, 6.0)
        self.assertGreater(metrics.std_luminance, 0.3)


if __name__ == "__main__":
    unittest.main()
