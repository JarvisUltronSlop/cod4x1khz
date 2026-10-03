"""Build/run isolated Win32 archive proposal contracts; never deploy a game."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

STAGE = Path(__file__).resolve().parent
PROJECT = STAGE.parent.parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--policy-only", action="store_true")
    selection.add_argument("--production-adapter-only", action="store_true")
    selection.add_argument("--probe-typecheck-only", action="store_true")
    selection.add_argument("--integration-typecheck-only", action="store_true")
    args = parser.parse_args()
    if args.integration_typecheck_only:
        # Copy exact proposed sources only under staged/archive; never apply.
        subprocess.run([sys.executable, str(STAGE / "prepare_production_integration.py")], check=True, cwd=STAGE)
    evidence = STAGE / "results" / datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    evidence.mkdir(parents=True)
    build = STAGE / "build"
    commands = []
    inputs = sorted(STAGE.glob("*.hpp")) + sorted(STAGE.glob("*.h")) + sorted(STAGE.glob("*.cpp"))
    inputs += [STAGE / "CMakeLists.txt", Path(__file__).resolve()]
    if args.production_adapter_only or args.probe_typecheck_only:
        inputs += [PROJECT / "engine/src/khz/khz_archive.cpp",
                   PROJECT / "engine/src/khz/khz_archive.h", PROJECT / "native/archive_history.hpp"]
    if args.probe_typecheck_only:
        inputs += [PROJECT / "engine/src/server_mp/sv_snapshot_profile_mp.cpp"] + sorted(STAGE.glob("*.inl"))
    if args.integration_typecheck_only:
        plan = json.loads((STAGE / "production_integration_plan.json").read_text(encoding="utf8"))
        inputs += [PROJECT / path for path in plan["production_sources"]]
        inputs += sorted(STAGE.glob("*.inl")) + sorted(STAGE.glob("*.py"))
        inputs += [STAGE / "production_integration_plan.json", STAGE / "production_integration.patch"]
        inputs += [path for path in (STAGE / "proposed-native").rglob("*") if path.is_file()]
    before = {str(path.relative_to(PROJECT)): digest(path) for path in inputs}
    result = {"completed": False, "passed": False, "scope": "staged standalone policy/adapter contracts",
              "native_loaded_map_attestation": False, "native_codec_attestation": False,
              "full_world_replay_attestation": False, "production_applied": False,
              "evidence": str(evidence), "commands": commands, "source_sha256": before}
    if args.production_adapter_only:
        result.update(scope="production archive adapter body; native layouts with synthetic owner/decoder/current-state/error dependencies",
                      adapter_source="production", production_source=str(PROJECT / "engine/src/khz/khz_archive.cpp"))
    if args.probe_typecheck_only:
        result.update(scope="source-owned loaded-map codec probe fragments; isolated original-TU object compilation only",
                      typecheck_only=True, native_probe_applied=False)
    if args.integration_typecheck_only:
        result.update(scope="exact proposed native owner-TU copies and guarded private boundary; isolated objects only",
                      typecheck_only=True, native_probe_applied=False,
                      preparation_plan=str(STAGE / "production_integration_plan.json"))
    report = evidence / "result.json"
    report.write_text(json.dumps(result, indent=2), encoding="utf8")

    def execute(name, argv):
        started = time.perf_counter()
        process = subprocess.run(argv, cwd=STAGE, capture_output=True, text=True, errors="replace")
        log = evidence / f"{name}.log"
        log.write_text(process.stdout + process.stderr, encoding="utf8")
        record = {"name": name, "argv": [str(arg) for arg in argv], "cwd": str(STAGE),
                  "exit_code": process.returncode, "elapsed_seconds": time.perf_counter() - started,
                  "log": str(log), "log_sha256": digest(log)}
        commands.append(record)
        report.write_text(json.dumps(result, indent=2), encoding="utf8")
        print(f"{name}: exit={process.returncode}, log={log}", flush=True)
        if process.returncode:
            raise RuntimeError(f"{name} failed; inspect {log}")

    try:
        instance = "C:/BuildTools"
        cache = PROJECT / "build/Release/CMakeCache.txt"
        for line in cache.read_text(encoding="utf8").splitlines():
            if line.startswith("CMAKE_GENERATOR_INSTANCE:INTERNAL="):
                instance = line.split("=", 1)[1]
                break
        result["toolchain_instance"] = instance
        execute("configure", ["cmake", "-S", str(STAGE), "-B", str(build),
                              "-G", "Visual Studio 17 2022", "-A", "Win32",
                              f"-DCMAKE_GENERATOR_INSTANCE={instance}"])
        targets = (["archive_native_boundary_typecheck"] if args.integration_typecheck_only else
                   ["archive_native_probe_typecheck"] if args.probe_typecheck_only else
                   ["archive_production_adapter_contracts"] if args.production_adapter_only else ["archive_policy_contracts"])
        if not args.policy_only and not args.production_adapter_only and not args.probe_typecheck_only and not args.integration_typecheck_only and (STAGE / "native_adapter_tests.cpp").exists():
            targets.append("archive_adapter_contracts")
        execute("build", ["cmake", "--build", str(build), "--config", "Release",
                          "--target", *targets, "--parallel", "1"])
        if args.probe_typecheck_only:
            objects = sorted((build / "archive_native_probe_typecheck.dir" / "Release").glob("*.obj"))
            if len(objects) != 2:
                raise RuntimeError(f"Expected both original-TU probe objects, found {len(objects)}")
            result["object_sha256"] = {str(path.relative_to(PROJECT)): digest(path) for path in objects}
        if args.integration_typecheck_only:
            objects = sorted((build / "archive_native_boundary_typecheck.dir" / "Release").rglob("*.obj"))
            if len(objects) != len(plan["prepared_cpp"]):
                raise RuntimeError(f"Expected {len(plan['prepared_cpp'])} proposed native owner objects, found {len(objects)}")
            result["object_sha256"] = {str(path.relative_to(PROJECT)): digest(path) for path in objects}
        for target in ([] if args.probe_typecheck_only or args.integration_typecheck_only else targets):
            binary = build / "Release" / f"{target}.exe"
            result.setdefault("binary_sha256", {})[str(binary.relative_to(PROJECT))] = digest(binary)
            execute(target, [str(binary)])
        after = {str(path.relative_to(PROJECT)): digest(path) for path in inputs}
        result["source_stable"] = before == after
        if before != after:
            raise RuntimeError("A contract source changed while compiling/executing")
        result.update(completed=True, passed=True, targets=targets)
    except Exception as error:
        result.update(completed=True, passed=False, error=str(error))
        raise
    finally:
        report.write_text(json.dumps(result, indent=2), encoding="utf8")
        print(f"evidence={report}", flush=True)


if __name__ == "__main__":
    main()
