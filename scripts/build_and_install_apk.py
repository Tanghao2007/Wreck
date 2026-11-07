#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Adaptive Grading Studio - One-Click APK Builder & Installer
============================================================
Builds production-aligned, signed APK using minimal portable toolchain
and deploys to connected Android device via ADB.
"""

import os
import sys
import glob
import time
import shutil
import zipfile
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = PROJECT_ROOT / "tools"
BUILD_DIR = PROJECT_ROOT / "build"
STAGING_DIR = BUILD_DIR / "apk_staging"
OUTPUT_APK = PROJECT_ROOT / "Wreck-v2.1.apk"

# Tool paths
JAVA_HOME = TOOLS_DIR / "jdk-17"
KEYTOOL = JAVA_HOME / "bin" / "keytool.exe"
AAPT2 = TOOLS_DIR / "android-sdk" / "build-tools" / "34.0.0" / "aapt2.exe"
D8 = TOOLS_DIR / "android-sdk" / "build-tools" / "34.0.0" / "d8.bat"
ZIPALIGN = TOOLS_DIR / "android-sdk" / "build-tools" / "34.0.0" / "zipalign.exe"
APKSIGNER = TOOLS_DIR / "android-sdk" / "build-tools" / "34.0.0" / "apksigner.bat"
ANDROID_JAR = TOOLS_DIR / "android-sdk" / "platforms" / "android-34" / "android.jar"
KOTLINC = TOOLS_DIR / "kotlinc" / "bin" / "kotlinc.bat"
KOTLIN_STDLIB = TOOLS_DIR / "kotlinc" / "lib" / "kotlin-stdlib.jar"
ZIG = TOOLS_DIR / "zig-windows-x86_64-0.13.0" / "zig.exe"
ADB = TOOLS_DIR / "platform-tools" / "adb.exe"

def get_env():
    env = os.environ.copy()
    env["JAVA_HOME"] = str(JAVA_HOME)
    env["PATH"] = str(JAVA_HOME / "bin") + ";" + env["PATH"]
    return env

def run_step(name, cmd, cwd=None):
    start = time.perf_counter()
    print(f"\n>>> [{name}]")
    res = subprocess.run(cmd, env=get_env(), cwd=cwd or str(PROJECT_ROOT), capture_output=True, text=True, errors="replace")
    elapsed = (time.perf_counter() - start) * 1000.0
    if res.returncode != 0:
        print(f"[FAIL] {name} exited with code {res.returncode}")
        if res.stdout:
            print("STDOUT:\n", res.stdout)
        if res.stderr:
            print("STDERR:\n", res.stderr)
        sys.exit(1)
    print(f"  [PASS] Completed in {elapsed:.1f} ms")
    return res.stdout

def main():
    print("=" * 72)
    print("  Adaptive Grading Engine - APK Packaging & Installation Pipeline")
    print("=" * 72)

    # 1. Check tools
    for tool_name, tool_path in [
        ("Java Keytool", KEYTOOL),
        ("AAPT2", AAPT2),
        ("D8 Dexer", D8),
        ("Zipalign", ZIPALIGN),
        ("Apksigner", APKSIGNER),
        ("Android.jar", ANDROID_JAR),
        ("Kotlinc", KOTLINC),
        ("Kotlin Stdlib", KOTLIN_STDLIB)
    ]:
        if not tool_path.exists():
            print(f"[FATAL] Required tool missing: {tool_path}")
            sys.exit(1)

    # Clean staging
    if STAGING_DIR.exists():
        shutil.rmtree(STAGING_DIR)
    STAGING_DIR.mkdir(parents=True, exist_ok=True)
    classes_dir = STAGING_DIR / "classes"
    dex_dir = STAGING_DIR / "dex"
    classes_dir.mkdir(parents=True, exist_ok=True)
    dex_dir.mkdir(parents=True, exist_ok=True)

    # 2. Compile Kotlin Sources
    kt_sources = [str(p) for p in (PROJECT_ROOT / "android" / "java").rglob("*.kt")]
    print(f"Found {len(kt_sources)} Kotlin source files to compile.")
    kotlinc_cmd = [
        str(KOTLINC),
        "-cp", str(ANDROID_JAR),
        "-d", str(classes_dir)
    ] + kt_sources
    run_step("1/6 Compile Kotlin Sources", kotlinc_cmd)

    # 3. Dex with D8
    class_files = [str(p) for p in classes_dir.rglob("*.class")]
    d8_cmd = [
        str(D8),
        "--output", str(dex_dir),
        "--lib", str(ANDROID_JAR),
        str(KOTLIN_STDLIB)
    ] + class_files
    run_step("2/6 Dalvik Bytecode Generation (D8)", d8_cmd)

    # 4. Link AndroidManifest via AAPT2
    manifest_xml = PROJECT_ROOT / "android" / "AndroidManifest.xml"
    base_apk = STAGING_DIR / "base.apk"
    aapt_cmd = [
        str(AAPT2),
        "link",
        "-I", str(ANDROID_JAR),
        "--manifest", str(manifest_xml),
        "--min-sdk-version", "24",
        "--target-sdk-version", "35",
        "-o", str(base_apk)
    ]
    run_step("3/6 AndroidManifest & Resources Link (AAPT2)", aapt_cmd)

    # 5. Compile ARM64 Native Library
    print("\n>>> [4/7 Cross-Compile ARM64 Native Engine]")
    native_so = BUILD_DIR / "libadaptive_grading_jni.so"
    sysroot_lib = BUILD_DIR / "android_sysroot" / "lib"
    extra_libs = []
    if (sysroot_lib / "libm.so").exists():
        extra_libs.extend([
            str(sysroot_lib / "libm.so"),
            str(sysroot_lib / "liblog.so"),
            str(sysroot_lib / "libjnigraphics.so"),
            "-Wl,--no-as-needed"
        ])
    else:
        extra_libs.extend(["-lm", "-lpthread", "-ljnigraphics"])

    zig_cmd = [
        str(ZIG), "cc", "-target", "aarch64-linux-musl", "-O3", "-shared", "-fPIC",
        "-I", str(PROJECT_ROOT / "c_core" / "include"),
        "-I", str(PROJECT_ROOT / "android" / "jni"),
        "-o", str(native_so),
        str(PROJECT_ROOT / "android" / "jni" / "adaptive_grading_jni.c"),
        str(PROJECT_ROOT / "c_core" / "src" / "adaptive_grading.c"),
        str(PROJECT_ROOT / "c_core" / "src" / "adaptive_grading_neon.c"),
        str(PROJECT_ROOT / "c_core" / "src" / "adaptive_logger.c"),
        str(PROJECT_ROOT / "c_core" / "src" / "adaptive_storage.c"),
        str(PROJECT_ROOT / "c_core" / "src" / "adaptive_thread_pool.c")
    ] + extra_libs
    run_step("Compile libadaptive_grading_jni.so", zig_cmd)

    # 6. Assemble APK Archive
    print("\n>>> [5/7 Assemble APK Archive]")
    unaligned_apk = STAGING_DIR / "unaligned.apk"
    if not native_so.exists():
        print(f"[FATAL] Missing native library: {native_so}")
        sys.exit(1)

    with zipfile.ZipFile(str(base_apk), "r") as zin, zipfile.ZipFile(str(unaligned_apk), "w", compression=zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            zout.writestr(item, zin.read(item.filename))
        
        # Add classes.dex
        classes_dex = dex_dir / "classes.dex"
        zout.write(str(classes_dex), "classes.dex")
        
        # Add uncompressed ARM64 shared library (ZIP_STORED for page alignment)
        zout.write(str(native_so), "lib/arm64-v8a/libadaptive_grading_jni.so", compress_type=zipfile.ZIP_STORED)

    print(f"  [PASS] Assembled unaligned.apk ({os.path.getsize(unaligned_apk) / 1024:.1f} KB)")

    # 6. Zipalign (4-byte alignment)
    aligned_apk = STAGING_DIR / "aligned.apk"
    zipalign_cmd = [
        str(ZIPALIGN),
        "-f", "-p", "4",
        str(unaligned_apk),
        str(aligned_apk)
    ]
    run_step("5/6 4-Byte Memory Alignment (Zipalign)", zipalign_cmd)

    # 7. Sign APK with debug keystore
    keystore = BUILD_DIR / "debug.keystore"
    if not keystore.exists():
        keytool_cmd = [
            str(KEYTOOL),
            "-genkeypair", "-v",
            "-keystore", str(keystore),
            "-alias", "androiddebugkey",
            "-keyalg", "RSA",
            "-keysize", "2048",
            "-validity", "10000",
            "-storepass", "android",
            "-keypass", "android",
            "-dname", "CN=Adaptive Grading Debug,O=Antigravity,C=CN"
        ]
        run_step("Generate Debug Keystore", keytool_cmd)

    apksigner_cmd = [
        str(APKSIGNER),
        "sign",
        "--ks", str(keystore),
        "--ks-pass", "pass:android",
        "--key-pass", "pass:android",
        "--out", str(OUTPUT_APK),
        str(aligned_apk)
    ]
    run_step("6/6 APK Signature (v1 + v2 + v3 Schemes)", apksigner_cmd)

    # Verify signature
    verify_cmd = [str(APKSIGNER), "verify", "--verbose", str(OUTPUT_APK)]
    run_step("Verify APK Signature", verify_cmd)

    # 同步保留 AdaptiveGrading-v2.1.apk 镜像
    mirror_apk = PROJECT_ROOT / "AdaptiveGrading-v2.1.apk"
    shutil.copyfile(str(OUTPUT_APK), str(mirror_apk))

    apk_size_kb = os.path.getsize(OUTPUT_APK) / 1024.0
    print("\n" + "=" * 72)
    print("  APK BUILD SUCCESSFUL!")
    print("=" * 72)
    print(f"  Output Package: {OUTPUT_APK}")
    print(f"  Package Size:   {apk_size_kb:.1f} KB")
    print(f"  Target ABI:     arm64-v8a (OnePlus 13 / Snapdragon 8 Elite)")
    print(f"  Package ID:     com.antigravity.grading")
    print(f"  MainActivity:   com.antigravity.grading.ui.CameraActivity")
    print("=" * 72)

    # 8. Check ADB and install to phone
    if ADB.exists():
        res = subprocess.run([str(ADB), "devices"], capture_output=True, text=True)
        lines = [l.strip() for l in res.stdout.splitlines() if l.strip() and not l.startswith("*") and not l.startswith("List")]
        attached = [l.split()[0] for l in lines if "\tdevice" in l]

        if attached:
            dev_id = attached[0]
            print(f"\n[ADB DEPLOY] Installing APK to connected device ({dev_id})...")
            # Push and pm install for ColorOS / Android 15 compatibility
            remote_tmp = "/data/local/tmp/grading_app_install.apk"
            subprocess.run([str(ADB), "-s", dev_id, "push", str(OUTPUT_APK), remote_tmp], capture_output=True, text=True)
            install_res = subprocess.run([str(ADB), "-s", dev_id, "shell", f"pm install -r -t -g {remote_tmp}"], capture_output=True, text=True)
            subprocess.run([str(ADB), "-s", dev_id, "shell", f"rm -f {remote_tmp}"], capture_output=True, text=True)

            print("ADB Output:", install_res.stdout.strip())
            if "Success" in install_res.stdout:
                print(f"  >>> SUCCESS: App successfully installed on your phone!")
                print(f"  >>> Launching camera activity on screen...")
                launch_res = subprocess.run([
                    str(ADB), "-s", dev_id, "shell", "am", "start",
                    "-n", "com.antigravity.grading/com.antigravity.grading.ui.CameraActivity"
                ], capture_output=True, text=True)
                print("Launch Output:", launch_res.stdout.strip())
                print(f"\n  [OK] App is now running on your phone! Look at your phone screen to test.")
            else:
                print(f"[WARN] ADB install returned: {install_res.stderr or install_res.stdout}")
        else:
            print("\n[NOTE] No online ADB device detected. You can manually copy the APK to your phone.")

if __name__ == "__main__":
    main()
