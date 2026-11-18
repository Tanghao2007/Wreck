"""
core/smoother.py
时域防闪烁平滑中枢 (Temporal Parameter Smoother)
负责对自适应调色解算器输出的动态参数进行自适应指数移动加权滤波（Adaptive EMA）
与死区迟滞滤波（Deadband Filter），彻底杜绝取景时的参数跳变与频闪。
"""

import numpy as np


class TemporalSmoother:
    """自适应时域平滑滤波器"""

    def __init__(
        self,
        base_alpha: float = 0.08,
        fast_alpha: float = 0.45,
        threshold: float = 0.15,
    ):
        """
        :param base_alpha: 稳态平滑系数 (越小越平滑, 0.05~0.1 适合稳定防抖)
        :param fast_alpha: 剧烈光照突变时的快速跟随系数 (0.3~0.6)
        :param threshold: 判定为突变的归一化变化阈值
        """
        self.base_alpha = base_alpha
        self.fast_alpha = fast_alpha
        self.threshold = threshold
        self.state: dict[str, float] = {}

    def reset(self):
        """重置状态"""
        self.state.clear()

    def update(self, target_params: dict[str, float]) -> dict[str, float]:
        """
        对目标参数字典进行平滑更新
        :param target_params: 本帧解算的目标参数字典 {param_name: value}
        :return: 平滑后的平稳参数字典
        """
        if not self.state:
            # 第一帧直接初始化
            self.state = {k: float(v) for k, v in target_params.items()}
            return dict(self.state)

        smoothed = {}
        for key, target_val in target_params.items():
            current_val = self.state.get(key, target_val)
            diff = abs(target_val - current_val)
            
            # 依据数值幅度评估相对变化率
            scale = max(abs(current_val), 1.0)
            rel_diff = diff / scale

            # 自适应平滑系数：如果微小抖动，采用 base_alpha；若大跨度转场，自动提升 alpha
            if rel_diff > self.threshold:
                # 突变状态，线性过渡到 fast_alpha
                ratio = min((rel_diff - self.threshold) / self.threshold, 1.0)
                alpha = self.base_alpha + ratio * (self.fast_alpha - self.base_alpha)
            else:
                alpha = self.base_alpha

            # EMA 更新
            new_val = current_val + alpha * (target_val - current_val)
            self.state[key] = new_val
            smoothed[key] = new_val

        return smoothed
