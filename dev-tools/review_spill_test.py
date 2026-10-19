"""Build/run the actual spill container with AddressSanitizer; clean scratch."""
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / "dev-tools/reviews/2026-10-16/fixes"
includes = []
args = shlex.split(json.loads((ROOT / "out/build/compile_commands.json").read_text())[0]["command"])
for i, arg in enumerate(args):
    if arg.startswith("-I"): includes.append(arg)
    elif arg == "-isystem": includes.extend((arg, args[i + 1]))
with tempfile.TemporaryDirectory(prefix="slc-spill-test-") as tmp:
    binary = Path(tmp) / "test"
    built = subprocess.run(["clang++", "-std=c++20", "-O0", "-g", "-fsanitize=address",
        *includes, str(ROOT / "dev-tools/review_spill_test.cpp"),
        str(ROOT / "src/luxrays/utils/memspill.cpp"), "-o", str(binary)], capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary), str(Path(tmp) / "spill.bin")], capture_output=True, text=True, timeout=10)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "spill-fixed.log").write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    assert "AddressSanitizer:" not in result.stderr
    print(result.stdout)
