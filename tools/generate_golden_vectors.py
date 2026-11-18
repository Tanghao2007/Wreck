"""
tools/generate_golden_vectors.py
用于批量生成跨平台移植判定基准 Golden Test Vectors 的脚本。
覆盖：
1. 典型摄影场景 (人像肤色、暖调日落、日光、阴天平光、大光比逆光、低照度夜景霓虹)
2. 极限与边界场景 (纯黑、纯白、中性灰、高频棋盘格、极端色彩条、单像素脉冲噪点)
3. 异常长宽比与分辨率 (超宽画幅、超高纵向、720p高清)
4. 全部 15 种艺术风格求解参数基准 (全覆盖)
5. 时域平滑器 (Temporal Smoother) 突变与过渡状态序列
6. 像素级着色渲染参考输出 (包含原始 RAW/BMP 与渲染 RAW/BMP，杜绝压缩失真)
"""

import os
import sys
import json
import struct
import numpy as np
import cv2

# 添加项目根目录到 Python 路径
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from core.analyzer import SceneAnalyzer, SceneMetrics
from core.styles import STYLE_REGISTRY, BaseStyle, StyleParameters
from core.smoother import TemporalSmoother
from core.engine import GradingEngine


def save_bmp_uncompressed(filepath: str, bgr: np.ndarray):
    """保存无压缩 24-bit BMP 图像，便于 C 语言与原生测试直接读取"""
    h, w, c = bgr.shape
    row_padding = (4 - (w * 3) % 4) % 4
    image_size = (w * 3 + row_padding) * h
    file_size = 54 + image_size

    # BMP Header (14 bytes) + DIB Header (40 bytes)
    header = struct.pack(
        "<2sIHHI",
        b"BM",
        file_size,
        0, 0,
        54
    )
    dib = struct.pack(
        "<IiiHHIIiiII",
        40,
        w,
        h, # 正数表示从下到上
        1,
        24,
        0,
        image_size,
        2835, 2835,
        0, 0
    )

    with open(filepath, "wb") as f:
        f.write(header)
        f.write(dib)
        pad = b"\x00" * row_padding
        # BMP 是从底行到顶行存储
        for y in range(h - 1, -1, -1):
            f.write(bgr[y, :, :3].tobytes())
            if row_padding > 0:
                f.write(pad)


def create_synthetic_portrait(w=256, h=256) -> np.ndarray:
    """创建合成人像测试帧：包含显著亚洲人/高加索人肤色椭圆区域、深色头发与背景"""
    img = np.full((h, w, 3), [180, 160, 140], dtype=np.uint8) # 浅灰背景
    # 人脸椭圆 (Cr ~ 150, Cb ~ 115, Y ~ 170) -> BGR: B ~ 130, G ~ 160, R ~ 210
    cv2.ellipse(img, (w // 2, h // 2), (w // 4, h // 3), 0, 0, 360, (135, 165, 215), -1)
    # 头发 (深暗部)
    cv2.ellipse(img, (w // 2, h // 3), (w // 4 + 10, h // 5), 0, 180, 360, (25, 20, 20), -1)
    # 眼睛与嘴唇 (高红饱和)
    cv2.circle(img, (w // 2 - 25, h // 2 - 15), 6, (20, 20, 20), -1)
    cv2.circle(img, (w // 2 + 25, h // 2 - 15), 6, (20, 20, 20), -1)
    cv2.ellipse(img, (w // 2, h // 2 + 35), (20, 8), 0, 0, 360, (60, 70, 210), -1)
    return img


def create_synthetic_sunset(w=256, h=256) -> np.ndarray:
    """创建暖调日落测试帧：强暖色温，高 R，低 B"""
    y, x = np.mgrid[:h, :w]
    grad = y.astype(np.float32) / h
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[..., 0] = np.clip(30 + grad * 40, 0, 255).astype(np.uint8)   # Blue
    img[..., 1] = np.clip(100 + grad * 80, 0, 255).astype(np.uint8)  # Green
    img[..., 2] = np.clip(220 + grad * 35, 0, 255).astype(np.uint8)  # Red
    # 太阳高光圆盘
    cv2.circle(img, (w // 2, h // 3), 30, (210, 245, 255), -1)
    return img


def create_synthetic_high_dr(w=256, h=256) -> np.ndarray:
    """创建大光比逆光测试帧：包含天空过曝与地面深暗阴影"""
    img = np.zeros((h, w, 3), dtype=np.uint8)
    # 上半部分：极亮天空 (250~255)
    img[:h // 2, :] = 252
    # 下半部分：背光暗部 (5~15)
    img[h // 2:, :] = 8
    # 剪影主体
    cv2.rectangle(img, (w // 3, h // 4), (2 * w // 3, h), (4, 4, 4), -1)
    return img


def create_synthetic_night_neon(w=256, h=256) -> np.ndarray:
    """创建低照度夜景霓虹测试帧：平均照度 < 0.22，局部紫蓝/青霓虹点光源"""
    img = np.full((h, w, 3), 15, dtype=np.uint8)
    # 霓虹线条
    cv2.line(img, (20, 50), (230, 50), (255, 30, 220), 5)  # 洋红霓虹
    cv2.line(img, (20, 180), (230, 180), (255, 230, 20), 5) # 青蓝霓虹
    cv2.circle(img, (w // 2, h // 2), 25, (30, 255, 255), -1) # 暖黄路灯
    return img


def create_synthetic_sunny(w=256, h=256) -> np.ndarray:
    """创建晴朗日光测试帧"""
    img = np.full((h, w, 3), [220, 180, 120], dtype=np.uint8) # 蓝天
    img[h // 2:, :] = [40, 140, 50] # 绿草地
    cv2.rectangle(img, (w // 4, h // 3), (3 * w // 4, 3 * h // 4), (160, 170, 180), -1) # 建筑
    return img


def create_synthetic_overcast(w=256, h=256) -> np.ndarray:
    """创建阴天平光测试帧：高均值、低标准差"""
    np.random.seed(123)
    noise = np.random.normal(180, 15, (h, w, 3)).clip(0, 255).astype(np.uint8)
    return noise


def create_color_bars(w=256, h=256) -> np.ndarray:
    """标准测试彩条"""
    colors = [
        [255, 255, 255], # 白
        [0, 255, 255],   # 黄 (BGR)
        [255, 255, 0],   # 青 (BGR)
        [0, 255, 0],     # 绿
        [255, 0, 255],   # 洋红
        [0, 0, 255],     # 红
        [255, 0, 0],     # 蓝
        [0, 0, 0],       # 黑
    ]
    bar_w = w // len(colors)
    img = np.zeros((h, w, 3), dtype=np.uint8)
    for i, col in enumerate(colors):
        x0 = i * bar_w
        x1 = w if i == len(colors) - 1 else (i + 1) * bar_w
        img[:, x0:x1] = col
    return img


def create_checkerboard(w=256, h=256, block_size=16) -> np.ndarray:
    """高频反差棋盘格"""
    y, x = np.mgrid[:h, :w]
    cb = ((x // block_size) + (y // block_size)) % 2
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[cb == 1] = 255
    return img


def scene_metrics_to_dict(m: SceneMetrics) -> dict:
    return {
        "mean_luminance": round(float(m.mean_luminance), 6),
        "std_luminance": round(float(m.std_luminance), 6),
        "shadow_clip_ratio": round(float(m.shadow_clip_ratio), 6),
        "highlight_clip_ratio": round(float(m.highlight_clip_ratio), 6),
        "estimated_cct": round(float(m.estimated_cct), 3),
        "tint": round(float(m.tint), 6),
        "scene_label": m.scene_label,
        "dynamic_range_ev": round(float(m.dynamic_range_ev), 4),
    }


def style_params_to_dict(p: StyleParameters) -> dict:
    return {
        "contrast": round(float(p.contrast), 6),
        "exposure_bias": round(float(p.exposure_bias), 6),
        "saturation": round(float(p.saturation), 6),
        "black_lift": round(float(p.black_lift), 6),
        "white_clip": round(float(p.white_clip), 6),
        "toe_strength": round(float(p.toe_strength), 6),
        "shoulder_strength": round(float(p.shoulder_strength), 6),
        "grain_intensity": round(float(p.grain_intensity), 6),
        "vignette": round(float(p.vignette), 6),
        "shadow_tint": [round(float(x), 6) for x in p.shadow_tint],
        "mid_tint": [round(float(x), 6) for x in p.mid_tint],
        "highlight_tint": [round(float(x), 6) for x in p.highlight_tint],
        "protect_skin": 1 if p.protect_skin else 0,
        "is_monochrome": 1 if p.is_monochrome else 0,
        "denoise_strength": round(float(p.denoise_strength), 6),
        "clarity_strength": round(float(p.clarity_strength), 6),
    }


def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    gv_dir = os.path.join(base_dir, "tests", "golden_vectors")
    os.makedirs(gv_dir, exist_ok=True)
    print(f"[Golden Vectors Generator] 目标输出目录: {gv_dir}")

    # 1. 导出预生成的 1024x1024 胶片银盐颗粒池 (固定 seed=42)
    np.random.seed(42)
    grain_pool = np.random.normal(0.0, 1.0, (1024, 1024)).astype(np.float32)
    grain_path = os.path.join(gv_dir, "grain_pool_1024x1024_f32.raw")
    grain_pool.tofile(grain_path)
    print(f"  [1/6] 银盐颗粒池基准已导出: {grain_path} ({os.path.getsize(grain_path)} 字节)")

    # 2. 组装测试集用例库
    test_cases = {
        # 典型场景
        "scene_01_portrait": create_synthetic_portrait(256, 256),
        "scene_02_sunset": create_synthetic_sunset(256, 256),
        "scene_03_sunny": create_synthetic_sunny(256, 256),
        "scene_04_overcast": create_synthetic_overcast(256, 256),
        "scene_05_high_dr": create_synthetic_high_dr(256, 256),
        "scene_06_night_neon": create_synthetic_night_neon(256, 256),
        # 边界与异常
        "edge_pure_black": np.zeros((256, 256, 3), dtype=np.uint8),
        "edge_pure_white": np.full((256, 256, 3), 255, dtype=np.uint8),
        "edge_neutral_gray": np.full((256, 256, 3), 128, dtype=np.uint8),
        "edge_color_bars": create_color_bars(256, 256),
        "edge_checkerboard": create_checkerboard(256, 256, 16),
        # 异常分辨率与纵横比
        "aspect_wide_640x160": create_synthetic_sunset(640, 160),
        "aspect_tall_160x640": create_synthetic_portrait(160, 640),
        "res_standard_720p": create_synthetic_portrait(1280, 720),
    }

    analyzer = SceneAnalyzer(target_size=(256, 256))

    # 3. 生成 Level 1: SceneMetrics 基准并导出每个用例的原始 RAW
    print("  [2/6] 正在生成 Level 1: SceneMetrics 感知指标基准与输入图...")
    metrics_manifest = {}
    for case_name, img in test_cases.items():
        in_raw_path = os.path.join(gv_dir, f"{case_name}_input.raw")
        img.tofile(in_raw_path)
        metrics = analyzer.analyze(img)
        m_dict = scene_metrics_to_dict(metrics)
        metrics_manifest[case_name] = m_dict

    with open(os.path.join(gv_dir, "metrics_golden.json"), "w", encoding="utf-8") as f:
        json.dump(metrics_manifest, f, indent=2, ensure_ascii=False)

    # 4. 生成 Level 2: 15 款艺术风格求解参数基准 (全覆盖)
    print("  [3/6] 正在生成 Level 2: 15 款风格解算参数基准...")
    styles_manifest = {}
    for case_name, img in test_cases.items():
        metrics = analyzer.analyze(img)
        case_styles = {}
        for s_id, s_obj in STYLE_REGISTRY.items():
            params = s_obj.solve(metrics)
            case_styles[f"style_{s_id:02d}"] = style_params_to_dict(params)
        styles_manifest[case_name] = case_styles

    with open(os.path.join(gv_dir, "styles_golden.json"), "w", encoding="utf-8") as f:
        json.dump(styles_manifest, f, indent=2, ensure_ascii=False)

    # 5. 生成 Level 3: 时域平滑器 (Temporal Smoother) 序列基准
    print("  [4/6] 正在生成 Level 3: 时域平滑器状态转移基准...")
    smoother = TemporalSmoother(base_alpha=0.08, fast_alpha=0.45, threshold=0.15)
    sequence_data = []
    # 模拟 10 帧：前 4 帧平稳 -> 第 5 帧突变 -> 6-10 帧逐步收敛
    test_inputs = [
        {"contrast": 1.0, "exposure_bias": 0.0, "saturation": 1.0},
        {"contrast": 1.02, "exposure_bias": 0.01, "saturation": 1.01},
        {"contrast": 1.01, "exposure_bias": 0.0, "saturation": 0.99},
        {"contrast": 1.03, "exposure_bias": 0.02, "saturation": 1.02},
        {"contrast": 1.80, "exposure_bias": 0.90, "saturation": 1.50}, # 强烈突变
        {"contrast": 1.75, "exposure_bias": 0.85, "saturation": 1.48},
        {"contrast": 1.70, "exposure_bias": 0.80, "saturation": 1.45},
        {"contrast": 1.70, "exposure_bias": 0.80, "saturation": 1.45},
        {"contrast": 1.70, "exposure_bias": 0.80, "saturation": 1.45},
        {"contrast": 1.70, "exposure_bias": 0.80, "saturation": 1.45},
    ]

    for frame_idx, inp in enumerate(test_inputs):
        out = smoother.update(inp)
        sequence_data.append({
            "frame": frame_idx,
            "input": inp,
            "smoothed": {k: round(float(v), 6) for k, v in out.items()}
        })

    with open(os.path.join(gv_dir, "smoother_golden.json"), "w", encoding="utf-8") as f:
        json.dump(sequence_data, f, indent=2)

    # 6. 生成 Level 4: 完整渲染流水线图像像素基准 (包含 Raw 与 BMP)
    print("  [5/6] 正在生成 Level 4: 图像着色渲染参考输出基准...")
    render_cases = [
        ("scene_01_portrait", 1),   # 01. 自然真实 (肤色保护 + 去噪清晰度)
        ("scene_01_portrait", 3),   # 03. 韩系柔光 (高柔光 + 肤色)
        ("scene_02_sunset", 4),     # 04. 胶片复古 (颗粒 + 暗角 + 曲线)
        ("scene_02_sunset", 6),     # 06. 欧美青橙电影 (冷暖对比)
        ("scene_05_high_dr", 8),    # 08. 经典黑白 (单色 + 曲线)
        ("scene_06_night_neon", 11),# 11. 赛博朋克 (霓虹色调 + 饱和)
        ("scene_03_sunny", 14),     # 14. 商业时尚 (极限微反差 50MP)
        ("scene_05_high_dr", 15),   # 15. 风光HDR (大动态压制高光)
        ("edge_neutral_gray", 1),   # 中性灰基准
    ]

    render_manifest = []

    for case_name, style_id in render_cases:
        img_in = test_cases[case_name]
        h, w, c = img_in.shape
        engine = GradingEngine(default_style_id=style_id)
        # 单帧测试：重置平滑器确保第一帧输出确定性
        engine.smoother.reset()
        engine.set_style(style_id)

        # 运行 process_frame
        graded_out, m_out, p_out, _ = engine.process_frame(img_in)

        prefix = f"{case_name}_style_{style_id:02d}"
        in_raw = os.path.join(gv_dir, f"{prefix}_input.raw")
        out_raw = os.path.join(gv_dir, f"{prefix}_output.raw")
        in_bmp = os.path.join(gv_dir, f"{prefix}_input.bmp")
        out_bmp = os.path.join(gv_dir, f"{prefix}_output.bmp")

        # 导出未压缩字节流与便于人看的 BMP
        img_in.tofile(in_raw)
        graded_out.tofile(out_raw)
        save_bmp_uncompressed(in_bmp, img_in)
        save_bmp_uncompressed(out_bmp, graded_out)

        render_manifest.append({
            "case_name": case_name,
            "style_id": style_id,
            "width": w,
            "height": h,
            "input_raw": os.path.basename(in_raw),
            "output_raw": os.path.basename(out_raw),
            "input_bmp": os.path.basename(in_bmp),
            "output_bmp": os.path.basename(out_bmp),
            "metrics": scene_metrics_to_dict(m_out),
            "params": style_params_to_dict(p_out),
        })

    with open(os.path.join(gv_dir, "render_manifest.json"), "w", encoding="utf-8") as f:
        json.dump(render_manifest, f, indent=2, ensure_ascii=False)

    print(f"  [6/6] 全部 Golden Test Vectors 生成完毕，测试集完整就绪！")


if __name__ == "__main__":
    main()
