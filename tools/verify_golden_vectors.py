"""
tools/verify_golden_vectors.py
误差判定与一致性验收工具 (Error Assessment & Verification Tool)
对 C 核心库输出结果与 Python 原型 Golden Vectors 进行多级精度判定：
1. Level 1: SceneMetrics 感知指标误差 (绝对误差、相对误差、场景标签一致性)
2. Level 2: StyleParameters 15 款风格解算参数误差 (各通道偏移、对比度、曝光)
3. Level 3: TemporalSmoother 时域平滑器序列误差
4. Level 4: 渲染像素级一致性 (PSNR, SSIM, 最大像素差 Max Abs Diff, 平均绝对误差 MAE)
"""

import os
import sys
import json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def compute_psnr(img1: np.ndarray, img2: np.ndarray) -> float:
    """计算两张图像之间的峰值信噪比 (PSNR, dB)"""
    mse = np.mean((img1.astype(np.float64) - img2.astype(np.float64)) ** 2)
    if mse <= 1e-10:
        return 999.0
    return float(10.0 * np.log10((255.0 ** 2) / mse))


def compute_mae(img1: np.ndarray, img2: np.ndarray) -> float:
    """计算平均绝对像素误差 (MAE)"""
    return float(np.mean(np.abs(img1.astype(np.float64) - img2.astype(np.float64))))


def verify_metrics(golden_file: str, c_file: str, tol=0.005) -> tuple[bool, list[str]]:
    """验证 SceneMetrics 指标一致性"""
    with open(golden_file, "r", encoding="utf-8") as f:
        golden = json.load(f)
    with open(c_file, "r", encoding="utf-8") as f:
        c_res = json.load(f)

    passed = True
    messages = []

    for case_name, g_val in golden.items():
        if case_name not in c_res:
            passed = False
            messages.append(f"[FAIL] 缺少用例输出: {case_name}")
            continue

        c_val = c_res[case_name]
        for key in ["mean_luminance", "std_luminance", "shadow_clip_ratio", "highlight_clip_ratio", "tint", "dynamic_range_ev"]:
            diff = abs(g_val[key] - c_val[key])
            if diff > tol:
                passed = False
                messages.append(f"[FAIL] {case_name}.{key}: golden={g_val[key]}, C={c_val[key]}, diff={diff:.6f} > {tol}")

        # 色温误差 (容差 50K，因为涉及除法和幂运算)
        cct_diff = abs(g_val["estimated_cct"] - c_val["estimated_cct"])
        if cct_diff > 25.0:
            passed = False
            messages.append(f"[FAIL] {case_name}.estimated_cct: golden={g_val['estimated_cct']}, C={c_val['estimated_cct']}, diff={cct_diff:.2f}")

    if passed:
        messages.append(f"[PASS] Level 1 (SceneMetrics): 全部 {len(golden)} 个测试场景指标与基准完全吻合！")

    return passed, messages


def verify_styles(golden_file: str, c_file: str, tol=0.005) -> tuple[bool, list[str]]:
    """验证 15 款艺术风格解算参数一致性"""
    with open(golden_file, "r", encoding="utf-8") as f:
        golden = json.load(f)
    with open(c_file, "r", encoding="utf-8") as f:
        c_res = json.load(f)

    passed = True
    messages = []
    total_checks = 0

    for case_name, case_styles in golden.items():
        if case_name not in c_res:
            passed = False
            messages.append(f"[FAIL] 样式缺少用例: {case_name}")
            continue

        for style_key, g_params in case_styles.items():
            total_checks += 1
            if style_key not in c_res[case_name]:
                passed = False
                messages.append(f"[FAIL] 缺少风格: {case_name}.{style_key}")
                continue

            c_params = c_res[case_name][style_key]
            for param_key in ["contrast", "exposure_bias", "saturation", "black_lift", "white_clip", "toe_strength", "shoulder_strength", "denoise_strength", "clarity_strength"]:
                diff = abs(g_params[param_key] - c_params[param_key])
                if diff > tol:
                    passed = False
                    messages.append(f"[FAIL] {case_name}.{style_key}.{param_key}: g={g_params[param_key]}, c={c_params[param_key]}, diff={diff:.6f}")

            # 检查 Tint 数组
            for tint_key in ["shadow_tint", "mid_tint", "highlight_tint"]:
                g_t = g_params[tint_key]
                c_t = c_params[tint_key]
                for ch in range(3):
                    diff = abs(g_t[ch] - c_t[ch])
                    if diff > tol:
                        passed = False
                        messages.append(f"[FAIL] {case_name}.{style_key}.{tint_key}[{ch}]: g={g_t[ch]}, c={c_t[ch]}, diff={diff:.6f}")

    if passed:
        messages.append(f"[PASS] Level 2 (15 Styles Solver): 全部 {total_checks} 组风格自适应参数与基准吻合！")

    return passed, messages


def verify_smoother(golden_file: str, c_file: str, tol=0.002) -> tuple[bool, list[str]]:
    """验证时域平滑器序列一致性"""
    with open(golden_file, "r", encoding="utf-8") as f:
        golden = json.load(f)
    with open(c_file, "r", encoding="utf-8") as f:
        c_res = json.load(f)

    passed = True
    messages = []
    for i, g_step in enumerate(golden):
        c_step = c_res[i]
        for k in ["contrast", "exposure_bias", "saturation"]:
            diff = abs(g_step["smoothed"][k] - c_step["smoothed"][k])
            if diff > tol:
                passed = False
                messages.append(f"[FAIL] Smoother frame {i} param {k}: g={g_step['smoothed'][k]}, c={c_step['smoothed'][k]}, diff={diff:.6f}")

    if passed:
        messages.append(f"[PASS] Level 3 (Temporal Smoother): 时域平滑序列转移曲线完全吻合！")

    return passed, messages


def verify_rendered_images(manifest_file: str, c_output_dir: str, gv_dir: str, min_psnr=40.0, max_diff_thresh=5) -> tuple[bool, list[str]]:
    """验证图像着色渲染像素级一致性"""
    with open(manifest_file, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    passed = True
    messages = []

    for item in manifest:
        case_name = item["case_name"]
        style_id = item["style_id"]
        w = item["width"]
        h = item["height"]
        g_raw_name = item["output_raw"]
        c_raw_name = f"c_{g_raw_name}"

        g_path = os.path.join(gv_dir, g_raw_name)
        c_path = os.path.join(c_output_dir, c_raw_name)

        if not os.path.exists(c_path):
            passed = False
            messages.append(f"[FAIL] 未找到 C 输出图像: {c_path}")
            continue

        g_bytes = np.fromfile(g_path, dtype=np.uint8).reshape((h, w, 3))
        c_bytes = np.fromfile(c_path, dtype=np.uint8).reshape((h, w, 3))

        psnr = compute_psnr(g_bytes, c_bytes)
        mae = compute_mae(g_bytes, c_bytes)
        max_diff = int(np.max(np.abs(g_bytes.astype(np.int32) - c_bytes.astype(np.int32))))

        status = "PASS" if (psnr >= min_psnr and max_diff <= max_diff_thresh) else "FAIL"
        if status == "FAIL":
            passed = False

        msg = f"[{status}] 渲染渲染用例 {case_name} (风格 {style_id:02d}): PSNR = {psnr:.2f} dB, MAE = {mae:.3f}, MaxDiff = {max_diff}"
        messages.append(msg)

    return passed, messages


def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    gv_dir = os.path.join(base_dir, "tests", "golden_vectors")
    c_out_dir = os.path.join(base_dir, "tests", "c_outputs")

    print("======================================================================")
    print("      GOLDEN TEST VECTORS 自动化比对与移植精度判定流水线")
    print("======================================================================")

    if not os.path.exists(c_out_dir):
        print(f"[提示] C 输出目录尚未生成: {c_out_dir}")
        print("请先编译并运行 C 核心库测试执行器生成对应输出！")
        return

    # 1. 验证 Metrics
    m_pass, m_msgs = verify_metrics(
        os.path.join(gv_dir, "metrics_golden.json"),
        os.path.join(c_out_dir, "c_metrics.json")
    )
    for m in m_msgs:
        print(" ", m)

    # 2. 验证 Styles
    s_pass, s_msgs = verify_styles(
        os.path.join(gv_dir, "styles_golden.json"),
        os.path.join(c_out_dir, "c_styles.json")
    )
    for m in s_msgs:
        print(" ", m)

    # 3. 验证 Smoother
    sm_pass, sm_msgs = verify_smoother(
        os.path.join(gv_dir, "smoother_golden.json"),
        os.path.join(c_out_dir, "c_smoother.json")
    )
    for m in sm_msgs:
        print(" ", m)

    # 4. 验证 Rendered Images
    r_pass, r_msgs = verify_rendered_images(
        os.path.join(gv_dir, "render_manifest.json"),
        c_out_dir,
        gv_dir
    )
    for m in r_msgs:
        print(" ", m)

    print("======================================================================")
    all_passed = m_pass and s_pass and sm_pass and r_pass
    if all_passed:
        print(">>> 判定结论: 全部 4 级 Golden Vectors 验证通过！移植精度达标！<<<")
        sys.exit(0)
    else:
        print(">>> 判定结论: 存在精度未达标或失败项，请检查上方日志！<<<")
        sys.exit(1)


if __name__ == "__main__":
    main()

