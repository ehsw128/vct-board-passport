#!/usr/bin/env python3
"""Export the complete source tree and selected merged firmware for GitHub."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import zipfile


ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_DIRS = {
    ".git", ".agents", ".trae", ".vscode", ".idea", "__pycache__",
    "build", "build_app", "build_host", "build_v10", "managed_components",
}
EXCLUDED_FILES = {"sdkconfig", "sdkconfig.old", ".DS_Store"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_files() -> list[Path]:
    result: list[Path] = []
    for current, dirs, files in os.walk(ROOT, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d not in EXCLUDED_DIRS
                         and not (Path(current) / d).is_symlink())
        for name in sorted(files):
            path = Path(current) / name
            if name not in EXCLUDED_FILES and not path.is_symlink():
                result.append(path)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="v18")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    version = args.version
    output = args.output or ROOT.parent / f"VCT-Board-{version}-GitHub-Source.zip"
    output = output.resolve()
    if output.is_relative_to(ROOT):
        parser.error("output must be outside the project source tree")

    firmware_dir = ROOT / "build" / "flash"
    firmware = [
        firmware_dir / f"VCT-Board-{version}-full.bin",
        firmware_dir / f"VCT-Board-{version}-UI-preview-full.bin",
    ]
    for path in firmware:
        if not path.is_file():
            parser.error(f"missing firmware: {path}")

    files = source_files()
    package_root = f"VCT-Board-{version}"
    info = (
        f"VCT 看板 {version} GitHub 源码包\n\n"
        "包含完整工程源码、应用资源、测试、依赖锁文件和两份 0x0 合并固件。\n"
        "不含 .git 历史、本机配置、编译缓存及 managed_components。\n"
        "解压后使用 ESP-IDF 5.5.3 构建；组件管理器依据 dependencies.lock "
        "和组件清单恢复依赖。\n"
        "固件位于 firmware/；联网正式版与离线界面版请勿混用。\n"
        "上传 GitHub 时解压后提交工程目录，不要只上传此 zip。\n"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6, allowZip64=True) as archive:
        for path in files:
            relative = path.relative_to(ROOT).as_posix()
            archive.write(path, f"{package_root}/{relative}")
        archive.writestr(f"{package_root}/PACKAGE_INFO.txt", info)
        sums = []
        for path in firmware:
            archive.write(path, f"{package_root}/firmware/{path.name}")
            sums.append(f"{sha256(path)}  firmware/{path.name}")
        archive.writestr(f"{package_root}/SHA256SUMS.txt", "\n".join(sums) + "\n")

    with zipfile.ZipFile(output) as archive:
        bad = archive.testzip()
        if bad:
            raise RuntimeError(f"archive CRC check failed: {bad}")
        names = set(archive.namelist())
        required = {
            f"{package_root}/CMakeLists.txt",
            f"{package_root}/sdkconfig.defaults",
            f"{package_root}/dependencies.lock",
            f"{package_root}/main/app_ui.c",
            f"{package_root}/main/app_wifi.c",
            f"{package_root}/assets/fonts/noto_sc_16.c",
            *(f"{package_root}/firmware/{path.name}" for path in firmware),
        }
        missing = required - names
        if missing:
            raise RuntimeError(f"archive missing required files: {sorted(missing)}")
        for path in firmware:
            content = archive.read(f"{package_root}/firmware/{path.name}")
            if hashlib.sha256(content).hexdigest() != sha256(path):
                raise RuntimeError(f"firmware hash mismatch: {path.name}")

    print(f"Archive: {output}")
    print(f"Files: {len(files) + len(firmware) + 2}")
    print(f"Size: {output.stat().st_size:,} bytes")
    print(f"SHA256: {sha256(output)}")


if __name__ == "__main__":
    main()
