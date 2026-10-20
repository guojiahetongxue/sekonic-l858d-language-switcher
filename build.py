#!/usr/bin/env python3
"""Build a self-contained, ad-hoc signed universal macOS application.

Requires the Apple Command Line Tools. No downloads, devices or certificates.
This script compiles and packages; it never launches the device helper.
"""
from pathlib import Path
import hashlib
import json
import plistlib
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
DIST = ROOT / "dist"
VERSION = "0.4.0"
APP_NAME = "L858D Language Switcher"


def run(*arguments):
    print(" ".join(str(x) for x in arguments), flush=True)
    subprocess.run([str(x) for x in arguments], cwd=ROOT, check=True)


def package_icon(iconset, destination):
    # Modern ICNS stores PNG representations directly.
    representations = {
        b"icp4": "icon_16x16.png", b"icp5": "icon_32x32.png",
        b"icp6": "icon_32x32@2x.png", b"ic07": "icon_128x128.png",
        b"ic08": "icon_256x256.png", b"ic09": "icon_512x512.png",
        b"ic10": "icon_512x512@2x.png", b"ic11": "icon_16x16@2x.png",
        b"ic12": "icon_32x32@2x.png", b"ic13": "icon_128x128@2x.png",
        b"ic14": "icon_256x256@2x.png",
    }
    chunks = []
    for kind, filename in representations.items():
        data = (iconset / filename).read_bytes()
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError(f"Not a PNG: {filename}")
        chunks.append(kind + struct.pack(">I", len(data) + 8) + data)
    payload = b"".join(chunks)
    destination.write_bytes(b"icns" + struct.pack(">I", len(payload) + 8) + payload)


def main():
    BUILD.mkdir(exist_ok=True)
    DIST.mkdir(exist_ok=True)
    cache = BUILD / "ModuleCache"
    cache.mkdir(exist_ok=True)
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    core = sorted((ROOT / "Sources" / "DeviceCore").glob("*.c"))
    if not core or not (ROOT / "Sources" / "App.swift").exists():
        raise SystemExit("Source files are not complete.")
    binaries = {}
    for arch in ("arm64", "x86_64"):
        target = f"{arch}-apple-macos13.0"
        folder = BUILD / arch
        folder.mkdir(exist_ok=True)
        helper = folder / "sekonic-device"
        gui = folder / "L858DSwitcher"
        run("xcrun", "clang", "-target", target, "-isysroot", sdk,
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", ROOT / "Sources" / "DeviceCore", *core,
            "-framework", "IOKit", "-framework", "CoreFoundation", "-o", helper)
        run("xcrun", "swiftc", "-target", target, "-sdk", sdk,
            "-swift-version", "5", "-O", "-module-cache-path", cache,
            "-framework", "Cocoa", ROOT / "Sources" / "App.swift", "-o", gui)
        binaries[arch] = (helper, gui)
    app = DIST / f"{APP_NAME}.app"
    if app.exists():
        shutil.rmtree(app)
    macos = app / "Contents" / "MacOS"
    resources = app / "Contents" / "Resources"
    macos.mkdir(parents=True)
    resources.mkdir()
    for index, name in ((0, "sekonic-device"), (1, "L858DSwitcher")):
        run("lipo", "-create", binaries["arm64"][index], binaries["x86_64"][index], "-output", macos / name)
    run("xcrun", "swiftc", "-swift-version", "5", "-module-cache-path", cache,
        ROOT / "Scripts" / "MakeIcon.swift", "-o", BUILD / "make-icon")
    iconset = BUILD / "Switcher.iconset"
    run(BUILD / "make-icon", iconset)
    package_icon(iconset, resources / "AppIcon.icns")
    for doc in (ROOT / "Docs").glob("*.txt"):
        shutil.copy2(doc, resources / doc.name)
    shutil.copy2(ROOT / "LICENSE", resources / "LICENSE.txt")
    info = {
        "CFBundleName": APP_NAME,
        "CFBundleDisplayName": APP_NAME,
        "CFBundleIdentifier": "local.l858d.language-switcher",
        "CFBundleExecutable": "L858DSwitcher",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": VERSION,
        "CFBundleVersion": "4",
        "CFBundleIconFile": "AppIcon",
        "CFBundleDevelopmentRegion": "zh_CN",
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": "Independent utility. Not affiliated with SEKONIC.",
    }
    (app / "Contents" / "Info.plist").write_bytes(plistlib.dumps(info, sort_keys=False))
    # File-provider folders can add Finder metadata to newly built bundles.
    # Remove only metadata forbidden by codesign, never quarantine attributes.
    for attribute in ("com.apple.FinderInfo", "com.apple.ResourceFork"):
        run("xattr", "-dr", attribute, app)
    run("codesign", "--force", "--sign", "-", "--timestamp=none", macos / "sekonic-device")
    run("codesign", "--force", "--sign", "-", "--timestamp=none", app)
    run("codesign", "--verify", "--deep", "--strict", "--verbose=2", app)
    run("plutil", "-lint", app / "Contents" / "Info.plist")
    app_zip = DIST / f"L858D-Language-Switcher-v{VERSION}-macOS.zip"
    if app_zip.exists():
        app_zip.unlink()
    # One UTF-8-aware ZIP writer keeps non-ASCII resource names intact. The
    # generated bundle has no resource forks or symlinks; write() preserves
    # Unix executable modes, and the extracted signature is checked separately.
    with zipfile.ZipFile(app_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in [app, *sorted(app.rglob("*"))]:
            if path.is_symlink():
                raise ValueError(f"Unexpected symlink in generated app: {path}")
            archive.write(path, Path(app.name) / path.relative_to(app))
        # Keep instructions readable before the first application launch.
        for doc in sorted((ROOT / "Docs").glob("*.txt")):
            archive.write(doc, doc.name)
    source_zip = DIST / f"L858D-Language-Switcher-v{VERSION}-source.zip"
    with zipfile.ZipFile(source_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        include = [ROOT / "build.py", ROOT / "LICENSE", ROOT / ".gitignore"]
        for folder in ("Sources", "Scripts", "Docs", "Tests"):
            include += sorted((ROOT / folder).rglob("*"))
        for path in include:
            if path.is_file() and not path.name.startswith(".DS_Store") and "__pycache__" not in path.parts:
                archive.write(path, Path("L858D-Language-Switcher") / path.relative_to(ROOT))
    manifest = {
        "version": VERSION,
        "minimum_macos": "13.0",
        "architectures": ["arm64", "x86_64"],
        "signing": "ad-hoc; not Developer ID signed or notarized",
        "supported_device": "L-858D / L-858D-U, firmware 17, 18 or 19, explicit region codes 0000/0001/0002/0003 only",
        "supported_firmware": ["0017", "0018", "0019"],
        "supported_region_codes": ["0000", "0001", "0002", "0003"],
        "unsupported_firmware_policy": "No language writes. Use official SEKONIC Data Transfer Software to upgrade to firmware 19 before switching language.",
        "hardware_evidence": "One Japanese-region meter only. Chinese save/restart/readback verified on 17 and 19; 18 English save/readback and retained Chinese after downgrade/restart observed. Per-language limitations are detailed in the bundled compatibility document. Other regions have static protocol review only.",
        "additional_runtime_required": False,
        "firmware_files_bundled": False,
        "meter_languages": ["English", "Japanese", "Simplified Chinese"],
        "interface_languages": ["English", "Simplified Chinese"],
        "files": {p.name: {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                  for p in (app_zip, source_zip)},
    }
    (DIST / "release-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"\nBuilt {app}\nNo device commands were executed.")


if __name__ == "__main__":
    main()
