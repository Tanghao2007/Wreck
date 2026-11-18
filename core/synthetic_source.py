"""
core/synthetic_source.py
仿真相机流生成器 (Synthetic Camera Source)
在无外接物理摄像头或自动化测试场景下，动态生成高画质拟真摄影测试画面：
- 肖像人像场景（含典型人脸肤色、高光与头发暗部，用于验证肤色保护）
- 黄金时刻逆光日落（含大光比渐变天空、夕阳与水面反光）
- 城市街头与建筑高光暗影
- 阴天冷调街区
支持按快捷键 [T] 随时循环切换测试场景。
"""

import time
import numpy as np
import cv2


class SyntheticCameraSource:
    """仿真动态摄影测试镜头流"""

    def __init__(self, width: int = 1280, height: int = 720):
        self.width = width
        self.height = height
        self.scene_index = 0
        self.scene_names = [
            "人像肖像与肤色场景 (Portrait & Skin Tone)",
            "黄金时刻逆光日落 (Golden Hour Sunset)",
            "大光比街头建筑 (High-Contrast Street)",
            "阴天冷调与霓虹夜色 (Overcast & Neon)",
        ]

    def next_scene(self):
        """切换到下一个仿真摄影场景"""
        self.scene_index = (self.scene_index + 1) % len(self.scene_names)
        return self.scene_names[self.scene_index]

    def read_frame(self) -> np.ndarray:
        """生成并返回当前帧"""
        t = time.perf_counter()
        if self.scene_index == 0:
            return self._generate_portrait_scene(t)
        elif self.scene_index == 1:
            return self._generate_sunset_scene(t)
        elif self.scene_index == 2:
            return self._generate_street_scene(t)
        else:
            return self._generate_neon_scene(t)

    def _generate_portrait_scene(self, t: float) -> np.ndarray:
        """肖像与肤色场景"""
        frame = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        # 背景：柔和温润的浅蓝/暖灰室内虚化 (Bokeh)
        y, x = np.ogrid[:self.height, :self.width]
        bg_b = 140 + 30 * np.sin(x / 400.0)
        bg_g = 130 + 20 * np.cos(y / 300.0)
        bg_r = 120 + 25 * np.sin((x + y) / 500.0)
        frame[..., 0] = np.clip(bg_b, 0, 255).astype(np.uint8)
        frame[..., 1] = np.clip(bg_g, 0, 255).astype(np.uint8)
        frame[..., 2] = np.clip(bg_r, 0, 255).astype(np.uint8)

        # 头部人脸椭圆中心 (轻微呼吸晃动)
        cx = self.width // 2 + int(10 * np.sin(t * 0.8))
        cy = self.height // 2 + int(6 * np.cos(t * 0.8))

        # 绘制人像头部基础肤色 (自然亚洲人/高加索人肤色 B:140, G:175, R:225)
        cv2.ellipse(frame, (cx, cy), (160, 220), 0, 0, 360, (135, 170, 220), -1)
        # 面部受光高光 (脸颊与额头)
        cv2.ellipse(frame, (cx - 30, cy - 40), (50, 70), -15, 0, 360, (160, 195, 240), -1)
        # 阴影侧面
        cv2.ellipse(frame, (cx + 60, cy + 30), (50, 100), 20, 0, 360, (100, 135, 185), -1)
        
        # 头发与肩颈暗部
        cv2.ellipse(frame, (cx, cy - 140), (180, 100), 0, 0, 360, (30, 25, 25), -1)
        cv2.rectangle(frame, (cx - 240, cy + 180), (cx + 240, self.height), (45, 50, 60), -1)

        # 轻度高斯模糊使边缘过渡自然具有景深感
        frame = cv2.GaussianBlur(frame, (15, 15), 0)
        return frame

    def _generate_sunset_scene(self, t: float) -> np.ndarray:
        """黄金时刻逆光日落大光比场景"""
        frame = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        # 渐变天空：从顶部深紫蓝到底部耀眼金黄
        y = np.linspace(0.0, 1.0, self.height, dtype=np.float32)
        sky_b = (200 * (1.0 - y) ** 2.0 + 30 * y).astype(np.uint8)
        sky_g = (80 * (1.0 - y) + 160 * y).astype(np.uint8)
        sky_r = (40 * (1.0 - y) + 245 * y).astype(np.uint8)

        frame[..., 0] = np.tile(sky_b[:, None], (1, self.width))
        frame[..., 1] = np.tile(sky_g[:, None], (1, self.width))
        frame[..., 2] = np.tile(sky_r[:, None], (1, self.width))

        # 太阳高光圆盘与光晕
        sun_x = int(self.width * 0.65)
        sun_y = int(self.height * 0.45)
        cv2.circle(frame, (sun_x, sun_y), 120, (120, 220, 255), -1)
        cv2.circle(frame, (sun_x, sun_y), 45, (255, 255, 255), -1)

        # 地平线与远山剪影 (深暗部)
        cv2.rectangle(frame, (0, int(self.height * 0.65)), (self.width, self.height), (20, 30, 40), -1)
        return frame

    def _generate_street_scene(self, t: float) -> np.ndarray:
        """大光比街头建筑"""
        frame = np.full((self.height, self.width, 3), 160, dtype=np.uint8)
        # 强烈的阳光照射高光墙面
        cv2.rectangle(frame, (100, 80), (600, self.height), (230, 235, 240), -1)
        # 阴暗狭窄的小巷深度阴影
        cv2.rectangle(frame, (600, 120), (self.width - 100, self.height), (15, 18, 22), -1)
        # 窗户格子与街头细节
        for row in range(4):
            for col in range(3):
                cv2.rectangle(frame, (160 + col * 120, 140 + row * 110),
                              (240 + col * 120, 210 + row * 110), (35, 45, 55), -1)
        return frame

    def _generate_neon_scene(self, t: float) -> np.ndarray:
        """夜景冷调与霓虹灯光"""
        frame = np.full((self.height, self.width, 3), 20, dtype=np.uint8)
        # 冷色背景地面反光
        frame[self.height // 2:, ...] = (40, 25, 20)
        # 亮粉色与青蓝色霓虹招牌
        cv2.putText(frame, "CYBER LIGHTS", (120, 260), cv2.FONT_HERSHEY_DUPLEX, 2.0, (255, 80, 220), 4)
        cv2.putText(frame, "TOKYO NIGHTS", (120, 420), cv2.FONT_HERSHEY_DUPLEX, 1.8, (230, 220, 40), 4)
        return frame
