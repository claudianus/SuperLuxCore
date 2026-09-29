#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Assemble a dev pysuperluxcore wheel from the Windows Release install tree.

Windows counterpart of make_dev_wheel.py: takes the already-built
Release install artifacts (pysuperluxcore.pyd + runtime DLLs) + python/
packages and packs them into a proper wheel (dist-info + RECORD) that
Blender's extension installer / luxloader can consume.

The layout mirrors what the release pipeline produces
(delvewheel repair + `make.bat win-recompose`):
  pysuperluxcore/            python sources + pysuperluxcore.pyd
  pysuperluxcore.libs/       runtime DLLs + LuxOpenImageDenoise_device_cpu.dll
                             + oidnDenoise.exe
  pysuperluxcore-<ver>.dist-info/

The delvewheel add_dll_directory() shim is prepended to
pysuperluxcore/__init__.py (same snippet sync_dev_install.ps1 injects).

Usage: dev-tools/make_dev_wheel_win.py [--repo DIR] [--install DIR]
"""

import argparse
import base64
import hashlib
import json
import sys
import zipfile
from pathlib import Path

PKG = "pysuperluxcore"
TAG = "cp313-cp313-win_amd64"

# Same shim sync_dev_install.ps1 injects into the installed __init__.py
# (what delvewheel generates in release wheels).
DLL_SHIM = """\
# dev-wheel DLL shim - replicate delvewheel's .libs registration.
import os as _os
from pathlib import Path as _Path
_libs = _Path(__file__).resolve().parent.parent / "pysuperluxcore.libs"
if _libs.is_dir():
    # Plain LoadLibrary("nvrtc64_120_0.dll") inside cuew ignores
    # AddDllDirectory dirs unless the default policy includes USER_DIRS;
    # LOAD_LIBRARY_SEARCH_DEFAULT_DIRS (0x1000) does (same as
    # pysuperluxcore's ensure_nvrtc).
    from ctypes import windll as _windll
    _windll.kernel32.SetDefaultDllDirectories(0x1000)
    _os.add_dll_directory(str(_libs))

"""

METADATA = """\
Metadata-Version: 2.2
Name: {pkg}
Version: {version}
Summary: LuxCore Python bindings
Keywords: raytracing,ray tracing,rendering,pbr,physical based rendering,path tracing
Author: SuperLuxCore contributors
Requires-Python: >=3.10
Requires-Dist: nvidia-cuda-nvrtc-cu12 == 12.9.86; sys_platform != 'darwin' and platform_machine != 'ARM64'
"""

WHEEL = """\
Wheel-Version: 1.0
Generator: make_dev_wheel_win 0.1
Root-Is-Purelib: false
Tag: {tag}
"""

ENTRY_POINTS = """\
[console_scripts]
{pkg}test = {pkg}test:main
{pkg}-console = {pkg}tools.console.cmd:main
{pkg}-maketx = {pkg}tools.maketx.cmd:main
{pkg}-merge = {pkg}tools.merge.cmd:main
{pkg}-netmenu = {pkg}tools.netmenu.cmd:main
{pkg}-netconsole = {pkg}tools.netconsole.cmd:main
{pkg}-netnode = {pkg}tools.netnode.cmd:main

[gui_scripts]
{pkg}-netconsole-ui = {pkg}tools.netconsole.ui:main
{pkg}-netnode-ui = {pkg}tools.netnode.ui:main
"""


def b64sha(data):
    d = hashlib.sha256(data).digest()
    return "sha256=" + base64.urlsafe_b64encode(d).rstrip(b"=").decode()


def main():
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=str(here))
    ap.add_argument("--install", default=None, help="install tree")
    ap.add_argument("--tag", default=TAG)
    ap.add_argument(
        "--no-nvrtc",
        action="store_true",
        help="do not bundle nvrtc*.dll (use with the nvidia-cuda-nvrtc-cu12 "
        "wheel, as CI wheels do)",
    )
    args = ap.parse_args()
    repo = Path(args.repo)
    install = Path(args.install) if args.install else repo / "out/install/Release"

    pyd = install / PKG / f"{PKG}.pyd"
    if not pyd.is_file():
        sys.exit(f"ERROR: no {PKG}.pyd in {install / PKG} - build the Release install first")

    bin_dir = install / "bin"
    dlls = sorted(
        d
        for d in bin_dir.glob("*.dll")
        if not (args.no_nvrtc and d.name.lower().startswith("nvrtc"))
    )
    if not dlls:
        sys.exit(f"ERROR: no runtime DLLs in {bin_dir}")

    # OIDN CPU device plugin must be named .dll in the wheel layout (OIDN
    # loads it by filename - see make.bat win-recompose). Recent install
    # trees ship the .dll in bin/ already; otherwise fall back to renaming
    # the .pyd from pysuperluxcore.libs/.
    oidn_dev_dll = "LuxOpenImageDenoise_device_cpu.dll"
    oidn_dev_pyd = install / f"{PKG}.libs" / "LuxOpenImageDenoise_device_cpu.pyd"
    have_oidn_dev = any(d.name == oidn_dev_dll for d in dlls)
    if not have_oidn_dev and not oidn_dev_pyd.is_file():
        sys.exit(f"ERROR: no {oidn_dev_dll} in {bin_dir} and no {oidn_dev_pyd}")

    # oidnDenoise.exe ships in .libs so pysuperluxcore.path_to_oidn() finds it
    oidn_exes = list(
        (repo / "out/dependencies/full_deploy/host").rglob("oidnDenoise.exe")
    )

    version_file = repo / "build-system" / "build-settings.json"
    version = ".".join(
        json.loads(version_file.read_text())["DefaultVersion"][k]
        for k in ("major", "minor", "patch")
    )

    out_dir = install / "wheel"
    out_dir.mkdir(parents=True, exist_ok=True)
    wheel_path = out_dir / f"{PKG}-{version}-{args.tag}.whl"

    records = []

    def add(zf, arcname, data):
        zf.writestr(arcname, data)
        records.append((arcname, b64sha(data), len(data)))

    def add_file(zf, arcname, path):
        add(zf, arcname, Path(path).read_bytes())

    with zipfile.ZipFile(wheel_path, "w", zipfile.ZIP_DEFLATED) as zf:
        # pysuperluxcore package: python sources with shimmed __init__
        pkg_src = repo / "python" / PKG
        for f in sorted(pkg_src.rglob("*")):
            if f.is_file() and "__pycache__" not in str(f):
                arcname = f"{PKG}/{f.relative_to(pkg_src).as_posix()}"
                if f.name == "__init__.py":
                    add(zf, arcname, DLL_SHIM.encode() + f.read_bytes())
                else:
                    add_file(zf, arcname, f)
        add_file(zf, f"{PKG}/{PKG}.pyd", pyd)

        # tools + test packages
        for name in (f"{PKG}tools", f"{PKG}test"):
            src = repo / "python" / name
            for f in sorted(src.rglob("*")):
                if f.is_file() and "__pycache__" not in str(f):
                    add_file(zf, f"{name}/{f.relative_to(src).as_posix()}", f)

        # Runtime DLLs -> sibling .libs dir (delvewheel layout)
        for dll in dlls:
            add_file(zf, f"{PKG}.libs/{dll.name}", dll)
        if not have_oidn_dev:
            add_file(zf, f"{PKG}.libs/{oidn_dev_dll}", oidn_dev_pyd)
        if oidn_exes:
            add_file(zf, f"{PKG}.libs/oidnDenoise.exe", oidn_exes[0])
        else:
            print("WARNING: oidnDenoise.exe not found in deps tree - "
                  "external OIDN denoiser won't be available")

        # dist-info
        di = f"{PKG}-{version}.dist-info"
        add(zf, f"{di}/METADATA", METADATA.format(pkg=PKG, version=version).encode())
        add(zf, f"{di}/WHEEL", WHEEL.format(tag=args.tag).encode())
        add(zf, f"{di}/entry_points.txt", ENTRY_POINTS.format(pkg=PKG).encode())
        for lic in ("COPYING.txt", "AUTHORS.txt"):
            p = repo / lic
            if p.is_file():
                add_file(zf, f"{di}/licenses/{lic}", p)
        record = "".join(f"{n},{h},{s}\n" for n, h, s in records)
        record += f"{di}/RECORD,,\n"
        zf.writestr(f"{di}/RECORD", record)

    print(f"wheel: {wheel_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
