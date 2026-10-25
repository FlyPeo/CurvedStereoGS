# -*- coding: utf-8 -*-
"""Build a self-contained Windows package at _bin/CurvedStereoGS_Portable."""

from pathlib import Path
import shutil
import sys


ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "_bin"
RELEASE = BIN / "Release"
PACKAGE = BIN / "CurvedStereoGS_Portable"
STAGING = BIN / "CurvedStereoGS_Portable.staging"


def copy_file(src: Path, dst: Path) -> None:
    if not src.is_file():
        raise FileNotFoundError(f"Required package file is missing: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def main() -> int:
    # Never replace an existing deliverable or erase a partial copy implicitly.
    if PACKAGE.exists():
        raise FileExistsError(f"Package already exists; move it aside first: {PACKAGE}")
    if STAGING.exists():
        raise FileExistsError(f"Staging directory already exists; inspect it first: {STAGING}")
    if not RELEASE.is_dir():
        raise FileNotFoundError(f"Release build not found: {RELEASE}")

    STAGING.mkdir(parents=True)
    try:
        # Executables and all app-local DLLs from the Release build, excluding
        # stale baseline builds, local settings, and logs.
        copy_file(RELEASE / "CurvedStereoGS.exe", STAGING / "CurvedStereoGS.exe")
        copy_file(RELEASE / "CurvedStereoGS_ControlCenter.exe", STAGING / "CurvedStereoGS_ControlCenter.exe")
        for src in RELEASE.glob("*.dll"):
            copy_file(src, STAGING / src.name)
        for folder in RELEASE.iterdir():
            if folder.is_dir():
                shutil.copytree(folder, STAGING / folder.name)

        # Include the Microsoft runtime locally so another PC does not need a
        # separate VC++ Redistributable installation.
        system32 = Path(r"C:\Windows\System32")
        for name in ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"):
            copy_file(system32 / name, STAGING / name)

        shutil.copytree(ROOT / "src" / "shaders", STAGING / "CurvedStereoGS_files" / "shaders")
        shutil.copytree(ROOT / "third_party" / "nvpro_core2" / "nvshaders", STAGING / "nvshaders")

        # Static example scenes and every runtime-ready 4DGS scene. Training
        # checkpoints and the original multi-gigabyte archive are not runtime assets.
        for src in (ROOT / "models").glob("*"):
            if src.is_file():
                copy_file(src, STAGING / "models" / src.name)
        runtime_assets = ROOT / "assets"
        for pattern in ("*.ply", "*.dgs"):
            for src in runtime_assets.glob(pattern):
                copy_file(src, STAGING / "assets" / src.name)
        for scene_dir in (runtime_assets / "training_cameras").iterdir():
            metadata = scene_dir / "train_meta.json"
            if metadata.is_file():
                copy_file(metadata, STAGING / "assets" / "training_cameras" / scene_dir.name / metadata.name)

        (STAGING / "启动控制面板.cmd").write_text(
            '@echo off\r\ncd /d "%~dp0"\r\nstart "CurvedStereoGS" "%~dp0CurvedStereoGS_ControlCenter.exe"\r\n',
            encoding="ascii",
        )
        (STAGING / "启动篮球4DGS渲染.cmd").write_text(
            '@echo off\r\ncd /d "%~dp0"\r\n'
            '"%~dp0CurvedStereoGS.exe" --headlessFrameCount 2000000000 '
            '--scene "%~dp0assets\\basketball_canonical.ply" '
            '--dynamicSequence "%~dp0assets\\basketball.dgs" '
            '--trainingCameraMetadata "%~dp0assets\\training_cameras\\basketball\\train_meta.json" '
            '--trainingCameraIndex 8\r\n',
            encoding="ascii",
        )
        (STAGING / "使用说明.txt").write_text(
            """CurvedStereoGS 便携版
======================

启动：双击“启动控制面板.cmd”，在场景列表选择模型并点击“启动渲染”。
也可以双击“启动篮球4DGS渲染.cmd”，从篮球训练视角直接启动动态渲染。

本目录已包含程序、运行库、着色器、静态模型、六组 4DGS 模型和训练相机数据。
请整体复制本文件夹并保持内部目录结构。无需安装 Python 或 Qt。

运行要求：Windows 10/11 64 位；支持 Vulkan 的显卡及较新的显卡驱动。
动态模型文件较大，复制整个文件夹需要足够的磁盘空间。

控制：鼠标左键旋转视角，中键平移，滚轮缩放；W/A/S/D 移动，Shift 加速，Ctrl 减速。
模型加载时会从中心向四周扩散，并由点状逐渐长成完整高斯基元。
""",
            encoding="utf-8",
        )

        # Record package contents and total size, including nested files.
        files = sorted(p for p in STAGING.rglob("*") if p.is_file())
        total = sum(p.stat().st_size for p in files)
        report = [
            f"Payload files (manifest excluded): {len(files)}",
            f"Payload size: {total:,} bytes ({total / (1024**3):.2f} GiB)",
        ]
        report.extend(f"{p.relative_to(STAGING)}\t{p.stat().st_size:,}" for p in files)
        (STAGING / "打包清单.txt").write_text("\n".join(report) + "\n", encoding="utf-8")

        # Same-volume rename publishes the completed package in one operation.
        STAGING.rename(PACKAGE)
        print(f"Package ready: {PACKAGE}")
        print(f"Files: {len(files)}; size: {total / (1024**3):.2f} GiB")
        return 0
    except Exception:
        # Preserve staging contents for inspection/recovery; never delete them.
        print(f"Packaging failed; partial files were preserved at: {STAGING}", file=sys.stderr)
        raise


if __name__ == "__main__":
    raise SystemExit(main())
