#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Assemble a dev pysuperluxcore wheel from the Release build tree.

Fast path for Blender dev installs: takes the already-built
Release/pysuperluxcore .so + python/ packages + bundled dylibs and packs
them into a proper wheel (dist-info + RECORD) that luxloader can install.

The full release pipeline is `python -m build-system.luxmake wheel-test`
(Debug, repairwheel) — this script only repacks existing artifacts.

Usage: dev-tools/make_dev_wheel.py [--repo DIR] [--build DIR]
"""

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

VERSION = "2.11.2"
TAG = "cp313-cp313-macosx_14_0_arm64"
PKG = "pysuperluxcore"

METADATA = f"""\
Metadata-Version: 2.2
Name: {PKG}
Version: {VERSION}
Summary: SuperLuxCore Python bindings
Keywords: raytracing,ray tracing,rendering,pbr,physical based rendering,path tracing
Author: SuperLuxCore contributors
Requires-Python: >=3.10
Requires-Dist: nvidia-cuda-nvrtc-cu12 == 12.8.93; sys_platform != 'darwin' and platform_machine != 'ARM64'
"""

WHEEL = f"""\
Wheel-Version: 1.0
Generator: make_dev_wheel 0.1
Root-Is-Purelib: false
Tag: {TAG}
"""

ENTRY_POINTS = f"""\
[console_scripts]
{PKG}test = {PKG}test:main
{PKG}-console = {PKG}tools.console.cmd:main
{PKG}-maketx = {PKG}tools.maketx.cmd:main
{PKG}-merge = {PKG}tools.merge.cmd:main
{PKG}-netmenu = {PKG}tools.netmenu.cmd:main
{PKG}-netconsole = {PKG}tools.netconsole.cmd:main
{PKG}-netnode = {PKG}tools.netnode.cmd:main

[gui_scripts]
{PKG}-netconsole-ui = {PKG}tools.netconsole.ui:main
{PKG}-netnode-ui = {PKG}tools.netnode.ui:main
"""


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def collect_dylibs(so_path, lib_dir):
    """Mirror sync_dev_install.sh: rewrite @rpath deps -> @loader_path/.dylibs."""
    # rpath dirs from the .so itself, then install lib
    lines = run(["otool", "-l", str(so_path)]).stdout.splitlines()
    rpaths = [
        lines[i + 2].split("path ")[1].split(" (")[0].strip()
        for i, line in enumerate(lines)
        if "LC_RPATH" in line
    ]
    search = [d for d in rpaths if os.path.isdir(d)] + [lib_dir]
    deps = [
        l.split()[0].strip()
        for l in run(["otool", "-L", str(so_path)]).stdout.splitlines()
        if "@rpath/" in l
    ]
    dylibs = []
    for ref in deps:
        lib = os.path.basename(ref)
        for d in search:
            src = os.path.join(d, lib)
            if os.path.isfile(src):
                dylibs.append((ref, lib, src))
                break
    return dylibs


def fix_rpaths(so_path, dylibs_dir, dylibs):
    for ref, lib, _ in dylibs:
        run(["install_name_tool", "-change", ref, f"@loader_path/.dylibs/{lib}", str(so_path)])
    for f in Path(dylibs_dir).glob("*.dylib"):
        out = run(["otool", "-L", str(f)]).stdout
        for l in out.splitlines():
            if "@rpath/" in l:
                lib = os.path.basename(l.split()[0].strip())
                run(["install_name_tool", "-change", l.split()[0].strip(), f"@loader_path/{lib}", str(f)])
        run(["install_name_tool", "-id", f"@loader_path/{f.name}", str(f)])


def b64sha(data):
    d = hashlib.sha256(data).digest()
    return "sha256=" + base64.urlsafe_b64encode(d).rstrip(b"=").decode()


def main():
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=str(here))
    ap.add_argument("--build", default=None,
                    help="dir holding pysuperluxcore.cpython-*.so")
    ap.add_argument("--install", default=None, help="install tree (for lib/)")
    args = ap.parse_args()
    repo = Path(args.repo)
    build = Path(args.build) if args.build else repo / "out/build/src/pysuperluxcore/Release"
    install = Path(args.install) if args.install else repo / "out/install/Release"

    sos = list(build.glob(f"{PKG}.cpython-*.so"))
    if not sos:
        sys.exit(f"ERROR: no {PKG}.cpython-*.so in {build}")
    so = sos[0]
    lib_dir = install / "lib"

    out_dir = install / "wheel"
    out_dir.mkdir(parents=True, exist_ok=True)
    wheel_path = out_dir / f"{PKG}-{VERSION}-{TAG}.whl"

    records = []

    def add(zf, arcname, data):
        zf.writestr(arcname, data)
        records.append((arcname, b64sha(data), len(data)))

    def add_file(zf, arcname, path):
        add(zf, arcname, Path(path).read_bytes())

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        # .so + .dylibs with rewritten rpaths
        work = td / PKG
        dylib_dir = work / ".dylibs"
        dylib_dir.mkdir(parents=True)
        wso = work / so.name
        shutil.copy2(so, wso)
        dylibs = collect_dylibs(wso, lib_dir)
        for _, lib, src in dylibs:
            shutil.copy2(src, dylib_dir / lib)
        fix_rpaths(wso, dylib_dir, dylibs)

        with zipfile.ZipFile(wheel_path, "w", zipfile.ZIP_DEFLATED) as zf:
            # pysuperluxcore package: python sources + .so + .dylibs
            pkg_src = repo / "python" / PKG
            for f in sorted(pkg_src.rglob("*")):
                if f.is_file() and "__pycache__" not in str(f):
                    add_file(zf, f"{PKG}/{f.relative_to(pkg_src)}", f)
            add_file(zf, f"{PKG}/{so.name}", wso)
            for f in sorted(dylib_dir.glob("*.dylib")):
                add_file(zf, f"{PKG}/.dylibs/{f.name}", f)
            # tools + test packages
            for name in (f"{PKG}tools", f"{PKG}test"):
                src = repo / "python" / name
                for f in sorted(src.rglob("*")):
                    if f.is_file() and "__pycache__" not in str(f):
                        add_file(zf, f"{name}/{f.relative_to(src)}", f)
            # dist-info
            di = f"{PKG}-{VERSION}.dist-info"
            add(zf, f"{di}/METADATA", METADATA.encode())
            add(zf, f"{di}/WHEEL", WHEEL.encode())
            add(zf, f"{di}/entry_points.txt", ENTRY_POINTS.encode())
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
