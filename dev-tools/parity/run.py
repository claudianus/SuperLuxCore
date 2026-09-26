# SPDX-License-Identifier: Apache-2.0
#
# CPU/GPU parity matrix runner (Phase 3.1): renders every registered
# scene on PATHCPU plus each selected GPU backend and reports
# mean-luminance ratio / RMSE / structural checks against per-scene
# thresholds.
#
#   python3.13 dev-tools/parity/run.py
#   python3.13 dev-tools/parity/run.py --scenes tilepath_lt,bevel_tex
#   python3.13 dev-tools/parity/run.py --backends metal
#   python3.13 dev-tools/parity/run.py --spp 32        # smoke pass

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from harness import (REPO, device_mask, load_scene, render, metrics,
        luminance, save_ppm)
from scenes import SCENES

OUT_DIR = REPO / "dev-tools/out/parity"

BACKENDS = {"metal": "METAL_GPU", "opencl": "OPENCL_GPU"}

# The CPU reference renders the same feature flags minus GPU-only knobs
# (light tracing runs as the native hybrid thread; the tile sampler is
# GPU-only so the reference falls back to SOBOL)
CPU_ONLY_SAMPLER_FALLBACK = {"TILEPATHSAMPLER": "SOBOL"}
GPU_ONLY_FLAGS = ("path.lighttracing.enable", "path.lighttracing.only",
        "path.lighttracing.taskfraction", "tile.size.")


def cpu_spec(spec):
    ref = dict(spec)
    # Optional estimator-family reference override: scenes exercising a
    # GPU feature PATHCPU can not match (e.g. vertex merging needs the
    # VCM engine) name their reference explicitly via "ref_engine";
    # "ref_cfg_extra" then replaces the GPU cfg entirely.
    ref["engine"] = spec.get("ref_engine", "PATHCPU")
    ref["sampler"] = CPU_ONLY_SAMPLER_FALLBACK.get(spec["sampler"],
            spec["sampler"])
    if "ref_cfg_extra" in spec:
        ref["cfg_extra"] = spec["ref_cfg_extra"]
    else:
        ref["cfg_extra"] = "\n".join(
                ln for ln in spec.get("cfg_extra", "").splitlines()
                if not any(ln.startswith(f) for f in GPU_ONLY_FLAGS)) + "\n"
    return ref


def check(name, backend, m, expect):
    fails = []
    if m["ratio"] < expect.get("ratio_min", 0.95):
        fails.append(f"ratio {m['ratio']:.4f} < {expect['ratio_min']}")
    if m["ratio"] > expect.get("ratio_max", 1.05):
        fails.append(f"ratio {m['ratio']:.4f} > "
                f"{expect.get('ratio_max', 1.05)}")
    if m["rmse"] > expect.get("rmse_max", 0.10):
        fails.append(f"rmse {m['rmse']:.5f} > {expect['rmse_max']}")
    if m["nans"]:
        fails.append(f"{m['nans']} NaNs")
    if m["black_frac"] > expect.get("black_max", 0.02):
        fails.append(f"black_frac {m['black_frac']:.4f} > "
                f"{expect['black_max']}")
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenes", help="comma list; default: all")
    ap.add_argument("--backends", default="metal,opencl",
            help="comma list of metal,opencl")
    ap.add_argument("--spp", type=int, help="override per-scene spp")
    ap.add_argument("--out", default=str(OUT_DIR))
    args = ap.parse_args()

    names = args.scenes.split(",") if args.scenes else list(SCENES)
    backends = [b for b in args.backends.split(",") if b in BACKENDS]
    masks = {b: device_mask(BACKENDS[b]) for b in backends}

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    report = []
    ratios = []
    for name in names:
        spec = dict(SCENES[name])
        if args.spp:
            spec["spp"] = args.spp
        scene = load_scene(spec)

        print(f"\n=== {name}: {spec.get('ref_engine', 'PATHCPU')} (ref) ===", flush=True)
        ref = render(scene, cpu_spec(spec), spp=spec["spp"])
        save_ppm(ref, out / f"{name}_cpu.ppm")
        print(f"  mean luminance {luminance(ref).mean():.5f}", flush=True)

        for b in backends:
            if not masks[b]:
                print(f"  skip {b}: no {BACKENDS[b]} device", flush=True)
                continue
            print(f"=== {name}: {spec['engine']} ({b}) ===", flush=True)
            img = render(scene, spec, sel=masks[b], spp=spec["spp"])
            save_ppm(img, out / f"{name}_{b}.ppm")
            m = metrics(ref, img)
            fails = check(name, b, m, spec.get("expect", {}))
            ratios.append(m["ratio"])
            status = "FAIL " + "; ".join(fails) if fails else "PASS"
            print(f"  {name}/{b}: ratio={m['ratio']:.4f} "
                    f"rmse={m['rmse']:.5f} nans={m['nans']} "
                    f"black={m['black_frac']:.4f}  {status}", flush=True)
            report.append((name, b, m, status))

    print("\n================ parity report ================")
    for name, b, m, status in report:
        print(f"{name:24s} {b:7s} ratio={m['ratio']:.4f} "
                f"rmse={m['rmse']:.5f}  {status}")
    if ratios:
        lo = min(ratios)
        # Systematic-bias detector: every backend below 1.0 by more than
        # the loosest scene threshold suggests global energy loss
        if all(r < 0.97 for r in ratios):
            print(f"WARNING: systematic low-energy bias on GPU "
                    f"(min ratio {lo:.4f})")
        if all(r > 1.03 for r in ratios):
            print(f"WARNING: systematic over-energy bias on GPU "
                    f"(max ratio {max(ratios):.4f})")
    failed = [r for r in report if r[3] != "PASS"]
    print(f"{len(report) - len(failed)}/{len(report)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
