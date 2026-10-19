"""Compile the actual emission-measure helper and check a path partition oracle."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='slc-bidir-measure-') as scratch:
    binary = Path(scratch)/'probe'
    subprocess.run(['clang++', '-std=c++20', '-O2', '-I'+str(root/'include'),
                    str(root/'dev-tools/bidir_measure_probe.cpp'), '-o', str(binary)],
                   check=True, capture_output=True, text=True, timeout=30)
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
    receipt = json.loads(result.stdout)
args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt))
