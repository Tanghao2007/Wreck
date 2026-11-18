"""
tests/test_smoother.py
时域防闪烁平滑中枢单元测试
"""

import unittest
from core.smoother import TemporalSmoother


class TestSmoother(unittest.TestCase):
    def test_initial_state(self):
        smoother = TemporalSmoother()
        initial = {"contrast": 1.2, "ev": 0.5}
        result = smoother.update(initial)
        self.assertEqual(result, initial)

    def test_jitter_suppression(self):
        """测试对微小扰动的高阻尼滤波（防频闪）"""
        smoother = TemporalSmoother(base_alpha=0.08)
        smoother.update({"contrast": 1.0})
        
        # 模拟高频微小抖动 (+/- 0.02)
        outputs = []
        for i in range(10):
            jitter_val = 1.0 + (0.02 if i % 2 == 0 else -0.02)
            out = smoother.update({"contrast": jitter_val})
            outputs.append(out["contrast"])
            
        # 平滑后的波动方差应该远小于原始抖动
        self.assertLess(max(outputs) - min(outputs), 0.015)

    def test_fast_tracking_on_scene_change(self):
        """测试在场景剧烈切换时能够快速平滑过渡收敛"""
        smoother = TemporalSmoother(base_alpha=0.08, fast_alpha=0.50, threshold=0.15)
        smoother.update({"ev": 0.0})
        
        # 突然从 0.0 变成 2.0
        val = 0.0
        for _ in range(8):
            out = smoother.update({"ev": 2.0})
            val = out["ev"]
            
        # 8 帧后应该已经接近 2.0 (> 1.6)
        self.assertGreater(val, 1.5)


if __name__ == "__main__":
    unittest.main()
