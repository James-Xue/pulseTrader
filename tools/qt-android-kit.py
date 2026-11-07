#!/usr/bin/env python3
"""Install and relocate a Qt 6.12.0 Android kit on Linux.

Why this exists
---------------
Qt 6.8+ publishes Android binaries under
  online/qtsdkrepository/all_os/android/qt6_6120/qt6_6120_<abi>/
instead of linux_x64/android/ (which stops at 6.7.3), so aqtinstall 3.3.0
reports "No architectures available" and cannot install them.

The published archives are build-host artifacts. Their payload is host-neutral
(POSIX shell wrappers in bin/, Android target libraries in lib/), but the
wrappers hardcode the Qt build machine's prefix. Depending on which machine Qt
was built on, that prefix is either /Users/qt/work/install (macOS) or
/home/qt/work/install (Linux).

This script downloads the base SDK archives, extracts them into
~/Qt/6.12.0/android_<abi>, and rebases every build-host path onto the local
desktop kit (~/Qt/6.12.0/gcc_64). After it runs, bin/qmake -query must print
QT_VERSION=6.12.0, QMAKE_XSPEC=android-clang and QT_HOST_PREFIX=<gcc_64>.

The Android SDK, NDK and a JDK are NOT installed by this script; they are
separate downloads (see docs/qt-client-design.md §4.5).
"""

import argparse
import re
import shutil
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

VERSION = "6.12.0"
VERSION_TAG_DIR = "qt6_6120"
QT_ROOT = Path.home() / "Qt" / VERSION
HOST_QT = QT_ROOT / "gcc_64"
CACHE = Path.home() / ".cache" / "qt-dl"

# Both observed build-host prefixes. Replacing only one leaves bin/qmake dead
# with "…/qmake6: not found" while CMake still works, which is easy to miss.
BUILD_HOST_PREFIXES = ("/Users/qt/work/install", "/home/qt/work/install")

ABIS = {
    "arm64-v8a": "arm64_v8a",
    "x86_64": "x86_64",
    "x86": "x86",
    "armeabi-v7a": "armv7",
}


def repo_base(suffix: str, mirror: str) -> str:
    return (
        f"{mirror.rstrip('/')}/online/qtsdkrepository/all_os/android/"
        f"{VERSION_TAG_DIR}/{VERSION_TAG_DIR}_{suffix}/"
    )


def fetch_manifest(base: str, package: str):
    """Return (version_tag, archive_names) for the base SDK package."""
    xml = urllib.request.urlopen(base + "Updates.xml", timeout=60).read()
    root = ET.fromstring(xml)
    node = next(p for p in root.findall("PackageUpdate") if p.findtext("Name") == package)
    archives = [
        a.strip()
        for a in (node.findtext("DownloadableArchives") or "").split(",")
        if a.strip()
    ]
    return node.findtext("Version"), archives


def download(base: str, package: str, version_tag: str, name: str) -> Path:
    dest = CACHE / f"{VERSION}-{package}" / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.stat().st_size > 0:
        print(f"  cached {name}", flush=True)
        return dest
    url = base + package + "/" + version_tag + name
    print(f"  downloading {name}", flush=True)
    with urllib.request.urlopen(url, timeout=600) as response, open(dest, "wb") as handle:
        shutil.copyfileobj(response, handle, 1 << 20)
    return dest


def relocate(target: Path) -> int:
    """Rebase build-host paths onto the local desktop kit."""
    changed = 0
    for path in target.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        if not any(prefix in text for prefix in BUILD_HOST_PREFIXES):
            continue
        for prefix in BUILD_HOST_PREFIXES:
            text = text.replace(prefix, str(HOST_QT))
        path.write_text(text, encoding="utf-8")
        changed += 1

    conf = target / "bin" / "target_qt.conf"
    if conf.exists():
        text = conf.read_text(encoding="utf-8")
        text = re.sub(r"^HostPrefix=.*$", "HostPrefix=../../gcc_64", text, flags=re.M)
        text = re.sub(r"^HostSpec=.*$", "HostSpec=linux-g++", text, flags=re.M)
        conf.write_text(text, encoding="utf-8")
        changed += 1
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("abi", choices=sorted(ABIS), help="Android ABI to install")
    parser.add_argument(
        "--mirror",
        default="https://download.qt.io",
        help="Qt repository mirror; the official host redirects to a slow "
             "regional mirror outside Europe, e.g. https://mirrors.aliyun.com/qt",
    )
    args = parser.parse_args()

    if not HOST_QT.exists():
        print(f"error: desktop kit not found at {HOST_QT}", file=sys.stderr)
        return 1

    suffix = ABIS[args.abi]
    target = QT_ROOT / f"android_{suffix}"
    base = repo_base(suffix, args.mirror)
    package = f"qt.qt6.6120.android_{suffix}"

    version_tag, archives = fetch_manifest(base, package)
    print(f"{package}: {len(archives)} archives (tag {version_tag})")

    import py7zr  # lazy import so --help works without the dependency

    for name in archives:
        archive = download(base, package, version_tag, name)
        with py7zr.SevenZipFile(archive) as handle:
            handle.extractall(path=target)
    print(f"extracted -> {target}")

    print(f"relocated {relocate(target)} files onto {HOST_QT}")

    qmake = target / "bin" / "qmake"
    if qmake.exists():
        result = subprocess.run(
            [str(qmake), "-query", "QT_VERSION"], capture_output=True, text=True
        )
        version = result.stdout.strip() or result.stderr.strip()
        print(f"qmake reports QT_VERSION = {version}")
        return 0 if version == VERSION else 1

    print("warning: bin/qmake missing; kit layout unexpected", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
