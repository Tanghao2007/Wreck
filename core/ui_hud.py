"""
core/ui_hud.py
现代化极简高质感 OSD (On-Screen Display) 仪表盘与视觉反馈模块
渲染：
- 顶部半透明拟态信息胶囊（当前大师风格、实时 FPS、场景识别类型、估计色温与动态范围）
- 底部操作指引徽章（1-5 选风格、S 分屏对比、Space 拍照、H 开关仪表盘）
- 调色引擎内部自适应动态参数微型监视条
- 快门按下时的高光曝光闪白动画与成片保存成功 Toast 提示
"""

import time
import numpy as np
import cv2
from core.analyzer import SceneMetrics
from core.styles import STYLE_REGISTRY, BaseStyle, StyleParameters


class HUDOverlay:
    """高品质相机取景界面与 HUD 渲染器"""

    def __init__(self):
        self.show_hud = True
        self.toast_message = ""
        self.toast_expire_time = 0.0
        self.shutter_flash_time = 0.0

    def trigger_shutter_animation(self, message: str = "成片已保存至 output/"):
        """触发快门闪光动画与 Toast 提示"""
        self.shutter_flash_time = time.perf_counter() + 0.15
        self.toast_message = message
        self.toast_expire_time = time.perf_counter() + 2.5

    def toggle_hud(self):
        """开关 HUD 覆盖显示"""
        self.show_hud = not self.show_hud

    def render(
        self,
        frame: np.ndarray,
        style: BaseStyle,
        metrics: SceneMetrics,
        params: StyleParameters,
        fps: float,
        split_mode: bool = False,
    ) -> np.ndarray:
        """在输出帧上绘制 HUD 覆盖层"""
        now = time.perf_counter()
        canvas = frame.copy()
        h, w = canvas.shape[:2]

        # 1. 快门闪白动画 (若刚按下快门)
        if now < self.shutter_flash_time:
            alpha = (self.shutter_flash_time - now) / 0.15
            white = np.full_like(canvas, 255)
            canvas = cv2.addWeighted(canvas, 1.0 - alpha * 0.7, white, alpha * 0.7, 0)

        if not self.show_hud:
            # 即使隐藏 HUD，Toast 提示仍应显示
            self._render_toast(canvas, now, w, h)
            return canvas

        overlay = canvas.copy()

        # 2. 顶部仪表盘背景（现代深灰半透明胶囊）
        top_bar_h = 48
        cv2.rectangle(overlay, (0, 0), (w, top_bar_h), (18, 18, 22), -1)

        # 3. 底部快捷键提示条背景
        bot_bar_h = 36
        cv2.rectangle(overlay, (0, h - bot_bar_h), (w, h), (18, 18, 22), -1)

        # 融合半透明背景 (alpha = 0.78)
        cv2.addWeighted(overlay, 0.78, canvas, 0.22, 0, canvas)

        # 4. 顶部文字绘制
        # 风格名称高亮徽章
        badge_text = f"[{style.id}/15] {style.name} ({style.english_name})"
        cv2.putText(canvas, badge_text, (18, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (80, 220, 255), 2, cv2.LINE_AA)

        # 帧率与场景
        fps_text = f"FPS: {fps:4.1f}"
        cv2.putText(canvas, fps_text, (w - 130, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.58, (120, 255, 120), 2, cv2.LINE_AA)

        # 居中显示场景感知情报 (色温/场景/DR)
        scene_info = f"{metrics.scene_label} | {int(metrics.estimated_cct)}K | DR: {metrics.dynamic_range_ev:.1f} EV"
        (tw, _), _ = cv2.getTextSize(scene_info, cv2.FONT_HERSHEY_SIMPLEX, 0.52, 1)
        center_x = max(int((w - tw) / 2), 350)
        cv2.putText(canvas, scene_info, (center_x, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (220, 220, 225), 1, cv2.LINE_AA)

        # 5. 分屏模式指示
        if split_mode:
            cv2.putText(canvas, "ORIGINAL", (24, 75), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (200, 200, 200), 1, cv2.LINE_AA)
            cv2.putText(canvas, "GRADED", (w - 110, 75), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (80, 220, 255), 2, cv2.LINE_AA)

        # 6. 底部快捷键指南
        tip_text = "[1-9,0] 直达风格1-10  |  [ , / . ] 循环切换15种审美  |  [S] 分屏  |  [SPACE] 拍照  |  [Q/ESC] 退出"
        cv2.putText(canvas, tip_text, (20, h - 12), cv2.FONT_HERSHEY_SIMPLEX, 0.44, (180, 185, 195), 1, cv2.LINE_AA)

        # 7. 渲染 Toast 提示
        self._render_toast(canvas, now, w, h)

        return canvas

    def _render_toast(self, canvas: np.ndarray, now: float, w: int, h: int):
        """渲染 Toast 浮层"""
        if now < self.toast_expire_time and self.toast_message:
            tw, th = 380, 44
            tx = (w - tw) // 2
            ty = h // 2 - th // 2
            
            overlay = canvas.copy()
            cv2.rectangle(overlay, (tx, ty), (tx + tw, ty + th), (25, 30, 40), -1)
            cv2.rectangle(overlay, (tx, ty), (tx + tw, ty + th), (60, 220, 120), 2)
            cv2.addWeighted(overlay, 0.88, canvas, 0.12, 0, canvas)
            
            cv2.putText(canvas, self.toast_message, (tx + 18, ty + 28),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (240, 255, 240), 1, cv2.LINE_AA)
