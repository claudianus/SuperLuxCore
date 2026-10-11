"""Exercise VDB extraction/destruction after Blender and Cycles load first.

This is a process compatibility regression, not an image quality test. A fresh
factory Blender imports Cycles, requires a supported host fixture to load, and
records candidate-fixture load attempts before importing the selected native
module. It repeatedly parses RGB/alpha density
grids and destroys their scenes. A SIGSEGV or timeout leaves durable, flushed
stage evidence, and the owner always terminates and reaps its process group.
No render, native build, installation, or persistent Blender settings change.
"""

from pathlib import Path
import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
import tempfile
import time


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def library_images():
    """Read dyld's actual image list, including Blender's TBB allocator."""
    import ctypes

    library = ctypes.CDLL(None)
    count = library._dyld_image_count
    count.restype = ctypes.c_uint32
    name = library._dyld_get_image_name
    name.argtypes = [ctypes.c_uint32]
    name.restype = ctypes.c_char_p
    result = []
    for i in range(count()):
        raw = name(i)
        if raw:
            path = Path(os.fsdecode(raw))
            if any(part in path.name.lower() for part in ("tbb", "vdb", "superlux")):
                result.append({"path": str(path), "sha256": sha(path) if path.is_file() else None})
    return result


def worker(args):
    import gc
    import importlib
    import traceback

    evidence = {"complete": False, "passed": False, "pid": os.getpid(),
                "scope": "Blender/Cycles first; repeated native VDB extraction; no render",
                "source_sha256": sha(__file__),
                "rounds_requested": args.rounds, "conversions_completed": 0,
                "fixtures": [{"path": str(p), "sha256": sha(p)}
                             for p in (args.fixture, args.rgba_fixture)]}
    journal_path = args.output.with_suffix(".stages.jsonl")
    worker_path = args.output.with_suffix(".worker.json")
    with journal_path.open("w", buffering=1) as journal:
        def stage(name, **fields):
            evidence["last_stage"] = name
            journal.write(json.dumps({"stage": name, "seconds_monotonic": time.monotonic(),
                                      **fields}, allow_nan=False) + "\n")
            journal.flush()
            write_json(worker_path, evidence)

        try:
            import bpy

            resources = Path(os.environ["BLENDER_USER_RESOURCES"]).resolve()
            expected_paths = {kind: Path(os.environ["BLENDER_USER_" + kind]).resolve()
                              for kind in ("CONFIG", "EXTENSIONS")}
            actual_paths = {kind: Path(bpy.utils.user_resource(kind)).resolve()
                            for kind in expected_paths}
            evidence["blender_user_paths"] = {
                "resources": str(resources),
                "expected": {kind: str(path) for kind, path in expected_paths.items()},
                "actual": {kind: str(path) for kind, path in actual_paths.items()},
            }
            for kind, expected in expected_paths.items():
                if (expected != resources / kind.lower() or not expected.is_dir()
                        or actual_paths[kind] != expected):
                    raise RuntimeError("Blender user path is not isolated: "
                                       + json.dumps(evidence["blender_user_paths"]))
            stage("isolated-blender-user-paths-verified")
            evidence["blender"] = {"version": bpy.app.version_string,
                                   "binary_path": bpy.app.binary_path,
                                   "binary_sha256": sha(bpy.app.binary_path)}
            if any(key.startswith("pysuperluxcore") for key in sys.modules):
                raise RuntimeError("Native already imported before Cycles-first prerequisite")
            stage("before-cycles-initialize")
            cycles = importlib.import_module("cycles")
            cycles.engine.init()
            import _cycles

            # Device enumeration does not build a Metal kernel or render.
            evidence["cycles_devices"] = [list(device) for device in _cycles.available_devices("NONE")]
            evidence["libraries_after_cycles"] = library_images()
            stage("cycles-initialized")
            host_fixture = (args.host_fixture or args.fixture).resolve()
            evidence["host_fixture"] = {"path": str(host_fixture), "sha256": sha(host_fixture)}
            loaded = []
            requested = [(host_fixture, True)]
            requested.extend((fixture.resolve(), False) for fixture in (args.fixture, args.rgba_fixture)
                             if fixture.resolve() != host_fixture)
            for fixture, required in requested:
                volume = bpy.data.volumes.new("load-order-" + fixture.stem)
                volume.filepath = str(fixture)
                stage("before-blender-vdb-load", fixture=str(fixture), required=required)
                record = {"path": str(fixture), "required_host_prerequisite": required}
                try:
                    volume.grids.load()
                    record.update(grids=[grid.name for grid in volume.grids],
                                  is_loaded=bool(volume.grids.is_loaded),
                                  error_message=getattr(volume.grids, "error_message", None))
                    record["loaded_successfully"] = record["is_loaded"] and bool(record["grids"])
                except Exception as error:
                    record.update(loaded_successfully=False, exception=repr(error))
                finally:
                    bpy.data.volumes.remove(volume)
                loaded.append(record)
                evidence["blender_loaded_vdb"] = loaded
                stage("blender-vdb-attempted-and-released", **record)
                if required and not record["loaded_successfully"]:
                    raise RuntimeError("Blender failed its supported host VDB prerequisite: " + json.dumps(record))
            evidence["blender_loaded_vdb"] = loaded
            evidence["libraries_after_blender_vdb"] = library_images()
            host_allocator = (Path(bpy.app.binary_path).parent.parent
                              / "Resources/lib/libtbbmalloc.dylib").resolve()
            if not host_allocator.is_file():
                raise RuntimeError("Blender bundled TBB allocator is missing")
            if not any(Path(item["path"]).resolve() == host_allocator
                       for item in evidence["libraries_after_blender_vdb"]):
                # TBB can defer loading its allocator until its first map
                # allocation. Preload the actual Blender allocator explicitly
                # to make the previously observed cross-runtime path present.
                import ctypes
                host_allocator_handle = ctypes.CDLL(str(host_allocator), mode=ctypes.RTLD_GLOBAL)
                evidence["host_allocator_preloaded"] = True
                evidence["host_allocator_preload_path"] = str(host_allocator)
            else:
                evidence["host_allocator_preloaded"] = False
            evidence["libraries_before_native"] = library_images()
            if not any(Path(item["path"]).resolve() == host_allocator
                       for item in evidence["libraries_before_native"]):
                raise RuntimeError("Blender TBB malloc library is absent; prerequisite unproven")
            stage("blender-host-allocator-loaded-before-native")
            if any(key.startswith("pysuperluxcore") for key in sys.modules):
                raise RuntimeError("Native imported during Blender/Cycles prerequisite")
            module_dir = args.module_dir.resolve()
            sys.path.insert(0, str(module_dir.parent if (module_dir / "__init__.py").is_file()
                                   else module_dir))
            stage("before-native-import")
            native = importlib.import_module("pysuperluxcore")
            extension = getattr(native, "pysuperluxcore", native)
            native_path = Path(extension.__file__).resolve()
            if not native_path.is_relative_to(module_dir):
                raise RuntimeError("Native was imported outside the requested directory: " + str(native_path))
            native_sha = sha(native_path)
            if args.expected_native_sha256 and native_sha != args.expected_native_sha256:
                raise RuntimeError("Native fingerprint differs from the requested candidate")
            evidence.update(native_path=str(native_path), native_sha256=native_sha,
                            libraries_after_native=library_images())
            native.Init()
            stage("native-initialized-after-cycles")
            grid_names = ["float-last-plane", "float-isolated", "float-negative", "float-tile-only",
                          "float-background", "float", "double", "int32", "int64", "bool",
                          "vec3f", "vec3d", "vec3i", "vec4f"]
            cases = [(args.fixture, name) for name in grid_names] + [(args.rgba_fixture, "rgba")]
            for repetition in range(args.rounds):
                for fixture, grid in cases:
                    # Metadata before Init is covered by the native contract. Here
                    # its active inclusive box defines only valid parser dimensions.
                    bounds = list(native.GetOpenVDBGridInfo(str(fixture), grid, True)[1])
                    if len(bounds) != 6:
                        raise RuntimeError("Unexpected active bounding box for " + grid)
                    for interpolation, padding in (("linear", 1), ("cubic", 3)):
                        for component in ("rgb", "alpha"):
                            label = {"round": repetition, "grid": grid,
                                     "interpolation": interpolation, "component": component}
                            props = native.Properties()
                            props.SetFromString("\n".join([
                                "scene.textures.test.type = densitygrid",
                                "scene.textures.test.storage = float",
                                "scene.textures.test.wrap = black",
                                "scene.textures.test.mapping.type = globalmapping3d",
                                "scene.textures.test.openvdb.mode = index",
                                "scene.textures.test.openvdb.file = " + json.dumps(str(fixture)),
                                "scene.textures.test.openvdb.grid = " + json.dumps(grid),
                                f"scene.textures.test.openvdb.padding = {padding}",
                                f"scene.textures.test.openvdb.component = {component}",
                                f"scene.textures.test.interpolation = {interpolation}",
                                *(f"scene.textures.test.n{axis} = {int(bounds[i + 3] - bounds[i] + 1 + 2 * padding)}"
                                  for i, axis in enumerate("xyz")),
                            ]))
                            scene = native.Scene()
                            stage("before-native-scene-parse", **label)
                            scene.Parse(props)
                            stage("native-scene-parse-returned", **label)
                            properties = scene.ToProperties()
                            if "scene.textures.test.type" not in properties.GetAllNames():
                                raise RuntimeError("Parsed texture absent from scene serialization")
                            del properties, scene, props
                            gc.collect()
                            evidence["conversions_completed"] += 1
                            stage("native-scene-and-properties-destroyed", **label)
            expected = args.rounds * len(cases) * 4
            if evidence["conversions_completed"] != expected:
                raise RuntimeError("Incomplete conversion count")
            evidence.update(complete=True, passed=True, conversions_expected=expected)
            stage("all-conversions-completed")
            print("BLENDER_VDB_LOAD_ORDER", json.dumps(evidence), flush=True)
            return 0
        except BaseException:
            evidence["exception"] = traceback.format_exc()
            stage("python-exception")
            traceback.print_exc()
            return 1


def owner(args):
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for suffix in (".worker.json", ".stages.jsonl"):
        args.output.with_suffix(suffix).unlink(missing_ok=True)
    args.output.unlink(missing_ok=True)
    started = time.monotonic()
    process = None
    failure = None
    with tempfile.TemporaryDirectory(prefix="slc-blender-vdb-load-order-") as temporary:
        config = Path(temporary) / "config"
        extensions = Path(temporary) / "extensions"
        config.mkdir()
        extensions.mkdir()
        env = os.environ.copy()
        env.update(OMP_NUM_THREADS="4", BLENDER_USER_RESOURCES=temporary,
                   BLENDER_USER_CONFIG=str(config), BLENDER_USER_EXTENSIONS=str(extensions),
                   TMPDIR=temporary)
        arguments = [str(args.blender), "--background", "--factory-startup", "--python-exit-code", "1",
                     "--python", str(Path(__file__).resolve()), "--", "--worker",
                     "--module-dir", str(args.module_dir.resolve()),
                     "--fixture", str(args.fixture.resolve()), "--rgba-fixture", str(args.rgba_fixture.resolve()),
                     "--output", str(args.output.resolve()), "--rounds", str(args.rounds)]
        if args.expected_native_sha256:
            arguments.extend(["--expected-native-sha256", args.expected_native_sha256])
        if args.host_fixture:
            arguments.extend(["--host-fixture", str(args.host_fixture.resolve())])
        with args.output.with_suffix(".runtime.log").open("w") as log:
            process = subprocess.Popen(arguments, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                process.wait(timeout=args.timeout)
            except BaseException as error:
                failure = repr(error)
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        process.wait(timeout=5)
        worker_path = args.output.with_suffix(".worker.json")
        evidence = {}
        if worker_path.is_file():
            try:
                evidence = json.loads(worker_path.read_text())
            except (OSError, json.JSONDecodeError) as error:
                evidence["worker_evidence_error"] = repr(error)
                journal_path = args.output.with_suffix(".stages.jsonl")
                if journal_path.is_file():
                    for line in journal_path.read_text().splitlines():
                        try:
                            evidence["last_stage"] = json.loads(line)["stage"]
                        except (json.JSONDecodeError, KeyError):
                            continue
        evidence.update(worker_exit_code=process.returncode, worker_pid=process.pid,
                        worker_terminal_and_reaped=process.poll() is not None,
                        worker_failure_signal=-process.returncode if process.returncode < 0 else None,
                        owner_elapsed_seconds=time.monotonic() - started,
                        owner_failure=failure, temporary_directory=temporary,
                        requested_module_dir=str(args.module_dir.resolve()),
                        source_sha256=sha(__file__), omp_num_threads=env["OMP_NUM_THREADS"])
    evidence["temporary_directory_removed"] = not Path(temporary).exists()
    evidence["passed"] = (evidence.get("complete") is True and process.returncode == 0
                          and failure is None and evidence["temporary_directory_removed"])
    write_json(args.output, evidence)
    print("BLENDER_VDB_LOAD_ORDER", json.dumps({key: evidence.get(key) for key in
          ("complete", "passed", "native_sha256", "conversions_completed", "worker_exit_code",
           "worker_terminal_and_reaped", "temporary_directory_removed")}), flush=True)
    return 0 if evidence["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender", type=Path,
                        default=Path("/Applications/Blender.app/Contents/MacOS/Blender"))
    parser.add_argument("--module-dir", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--rgba-fixture", type=Path, required=True)
    parser.add_argument("--host-fixture", type=Path,
                        help="Known Blender-supported grid fixture loaded before native; candidate fixtures are also probed")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-native-sha256")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    args = parser.parse_args(arguments)
    if not 1 <= args.rounds <= 8 or not 10 <= args.timeout <= 300:
        parser.error("Use 1–8 rounds and a 10–300 second timeout")
    for key in ("module_dir", "fixture", "rgba_fixture"):
        if not getattr(args, key).exists():
            parser.error(f"Missing {key}: {getattr(args, key)}")
    if args.host_fixture and not args.host_fixture.is_file():
        parser.error(f"Missing host_fixture: {args.host_fixture}")
    if not args.worker:
        def interrupted(signum, frame):
            raise InterruptedError(f"Received signal {signum}")
        signal.signal(signal.SIGTERM, interrupted)
        signal.signal(signal.SIGINT, interrupted)
    return worker(args) if args.worker else owner(args)


if __name__ == "__main__":
    sys.exit(main())
