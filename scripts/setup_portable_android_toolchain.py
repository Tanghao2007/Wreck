#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Setup Portable Lightweight Android & Kotlin Toolchain
=====================================================
Downloads and extracts minimal portable JDK 17, Android Build-Tools 34,
Android Platform 34 (android.jar), and Kotlin Compiler into `tools/`.
"""

import os
import sys
import time
import zipfile
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = PROJECT_ROOT / "tools"

COMPONENTS = [
    {
        "name": "Android Build-Tools 34.0.0",
        "url": "https://dl.google.com/android/repository/build-tools_r34-windows.zip",
        "zip_name": "build-tools.zip",
        "target_dir": TOOLS_DIR / "android-sdk" / "build-tools" / "34.0.0",
        "strip_parent": "android-14",  # Google build-tools zip has root folder 'android-14'
        "check_file": TOOLS_DIR / "android-sdk" / "build-tools" / "34.0.0" / "aapt2.exe"
    },
    {
        "name": "Android Platform 34 (android.jar)",
        "url": "https://dl.google.com/android/repository/platform-34-ext7_r03.zip",
        "zip_name": "platform.zip",
        "target_dir": TOOLS_DIR / "android-sdk" / "platforms" / "android-34",
        "strip_parent": "android-34",
        "check_file": TOOLS_DIR / "android-sdk" / "platforms" / "android-34" / "android.jar"
    },
    {
        "name": "Microsoft OpenJDK 17",
        "url": "https://download.visualstudio.microsoft.com/download/pr/58dca6c6-c3c9-4daa-8e17-b7d0df501afc/667a0e2c93e1aab8df0707a31c42d190/microsoft-jdk-17.0.12-windows-x64.zip",
        "zip_name": "jdk17.zip",
        "target_dir": TOOLS_DIR / "jdk-17",
        "strip_parent": "jdk-17.0.12+7",
        "check_file": TOOLS_DIR / "jdk-17" / "bin" / "java.exe"
    },
    {
        "name": "Kotlin Compiler 2.0.0",
        "url": "https://github.com/JetBrains/kotlin/releases/download/v2.0.0/kotlin-compiler-2.0.0.zip",
        "zip_name": "kotlinc.zip",
        "target_dir": TOOLS_DIR / "kotlinc",
        "strip_parent": "kotlinc",
        "check_file": TOOLS_DIR / "kotlinc" / "bin" / "kotlinc.bat"
    }
]

def download_file(url, dest_path):
    print(f"  Downloading from: {url}")
    headers = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req) as response, open(dest_path, "wb") as out_file:
        total_size = int(response.headers.get("Content-Length", 0))
        downloaded = 0
        chunk_size = 1024 * 1024  # 1MB chunks
        start_time = time.time()
        while True:
            chunk = response.read(chunk_size)
            if not chunk:
                break
            out_file.write(chunk)
            downloaded += len(chunk)
            elapsed = time.time() - start_time
            speed = (downloaded / (1024 * 1024)) / (elapsed + 1e-5)
            percent = (downloaded / total_size * 100) if total_size > 0 else 0
            mb_cur = downloaded / (1024 * 1024)
            mb_tot = total_size / (1024 * 1024)
            sys.stdout.write(f"\r    [{percent:5.1f}%] {mb_cur:.1f} / {mb_tot:.1f} MB ({speed:.1f} MB/s)")
            sys.stdout.flush()
    print()

def extract_zip(zip_path, target_dir, strip_parent=None):
    print(f"  Extracting to: {target_dir}...")
    target_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "r") as zf:
        members = zf.infolist()
        for member in members:
            orig_name = member.filename
            if member.is_dir() or orig_name.endswith("/"):
                continue

            parts = [p for p in orig_name.split("/") if p]
            if strip_parent and parts and parts[0] == strip_parent:
                parts = parts[1:]
            
            if not parts:
                continue

            rel_path = "/".join(parts)
            dest_file = target_dir / rel_path
            dest_file.parent.mkdir(parents=True, exist_ok=True)

            with zf.open(member) as src, open(dest_file, "wb") as dst:
                dst.write(src.read())

def main():
    print("=" * 72)
    print("  Setup Lightweight Portable Android & Kotlin Build Toolchain")
    print("=" * 72)
    TOOLS_DIR.mkdir(parents=True, exist_ok=True)

    for comp in COMPONENTS:
        name = comp["name"]
        check_file = comp["check_file"]
        if check_file.exists():
            print(f"[FOUND] {name} already installed: {check_file}")
            continue

        print(f"\n[INSTALLING] {name}...")
        zip_file = TOOLS_DIR / comp["zip_name"]
        try:
            download_file(comp["url"], zip_file)
            extract_zip(zip_file, comp["target_dir"], comp.get("strip_parent"))
            print(f"  [OK] Successfully configured {name}")
        finally:
            if zip_file.exists():
                try:
                    os.remove(zip_file)
                except Exception:
                    pass

    print("\n" * 1 + "=" * 72)
    print("  Toolchain Verification:")
    all_ready = True
    for comp in COMPONENTS:
        cf = comp["check_file"]
        status = "READY" if cf.exists() else "MISSING"
        if not cf.exists():
            all_ready = False
        print(f"  - {comp['name']:<35}: [{status}] ({cf.name})")
    print("=" * 72)

    if not all_ready:
        print("[ERROR] One or more components failed to install.")
        sys.exit(1)
    else:
        print("[SUCCESS] All portable tools are ready to build APK.")

if __name__ == "__main__":
    main()
