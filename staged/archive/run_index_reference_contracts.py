"""Generate/build/run source-extracted native reference-index contracts only.

Never change a production source or deploy/launch a game. Run only after the
exclusive timing fixture window closes. Preserve original native bodies and
layout; the two targets exercise legacy and production packed bit primitives.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

STAGE = Path(__file__).resolve().parent
PROJECT = STAGE.parent.parent
CONTRACT = STAGE / "reference-contracts"
NATIVE = PROJECT / "engine/src/qcommon"


def verify_proposal():
    """Check the narrow proposal retains the original oracle comparisons."""
    from prepare_reference_fix import proposed
    probe = PROJECT / "engine/src/khz/khz_archive_probe.inl"
    original = probe.read_text(encoding="utf-8-sig")
    changed = proposed(original)
    read_init = "MSG_InitReadOnly(&read, image->wire, wire.cursize); MSG_BeginReading(&read);"
    if original.count(read_init) != 2 or changed.count(read_init + "\n        MSG_ClearLastReferencedEntity(&read);") != 2:
        raise ValueError("Diagnostic proposal does not initialize both actual standalone readers")
    preserved = ["ok = !read.overflowed && client(expected, decoded->clients[0]);",
                 "ok = !read.overflowed && sameValue(expected.s.number, decoded->entities[0].s.number) &&",
                 "fields(&expected, &decoded->entities[0], archivedEntityFields, 69, sizeof(expected));"]
    for comparison in preserved:
        if original.count(comparison) != 1 or changed.count(comparison) != 1:
            raise ValueError("Refuse a changed original serialized/float oracle comparison")
    return probe, {"standalone_reader_resets": 2, "first_index_diagnostics": 2,
                   "original_comparisons_preserved": True, "native_codec_changed": False,
                   "proposed_source_sha256": hashlib.sha256(changed.encode("utf8")).hexdigest()}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def closing_brace(source, opening):
    """Find a C++ scope end without treating comment/string braces as scopes."""
    depth, index, state = 0, opening, "code"
    while index < len(source):
        ch = source[index]
        next_ch = source[index + 1] if index + 1 < len(source) else ""
        if state == "line":
            if ch == "\n":
                state = "code"
        elif state == "block":
            if ch == "*" and next_ch == "/":
                state, index = "code", index + 1
        elif state in ("string", "character"):
            if ch == "\\":
                index += 1
            elif ch == ('"' if state == "string" else "'"):
                state = "code"
        elif ch == "/" and next_ch in ("/", "*"):
            state, index = ("line" if next_ch == "/" else "block"), index + 1
        elif ch in ('"', "'"):
            state = "string" if ch == '"' else "character"
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    raise ValueError("Unterminated native scope; refuse extraction")


def extract_function(source, name):
    matches = list(re.finditer(r"^(?:void|int) __cdecl " + re.escape(name) + r"\s*\(", source, re.M))
    if len(matches) != 1:
        raise ValueError(f"Expected one native definition of {name}, found {len(matches)}")
    begin = matches[0].start()
    opening = source.index("{", begin)
    if ";" in source[begin:opening]:
        raise ValueError(f"Unexpected declaration before native {name} body")
    end = closing_brace(source, opening)
    return source[begin:end], source.count("\n", 0, begin) + 1


def extract_layout(source):
    matches = list(re.finditer(r"^struct msg_t\b", source, re.M))
    if len(matches) != 1:
        raise ValueError("Native message layout is no longer unique")
    begin = matches[0].start()
    end = closing_brace(source, source.index("{", begin))
    terminator = re.match(r"\s*;", source[end:])
    if not terminator:
        raise ValueError("Native message layout lacks its expected terminator")
    return source[begin:end + terminator.end()], source.count("\n", 0, begin) + 1


def generate():
    source_files = [NATIVE / name for name in ("msg_mp.h", "msg_mp.cpp", "msg_bits_mp.inc", "sv_msg_write_mp.cpp")]
    source_text = {path.name: path.read_text(encoding="utf-8-sig") for path in source_files}
    prefix = STAGE / "index_reference_fixture_prefix.inc"
    parts = [prefix.read_text(encoding="utf8")]
    layout, line = extract_layout(source_text["msg_mp.h"])
    parts += [layout, 'static_assert(sizeof(msg_t) == 0x28, "Native Win32 message layout changed");',
              source_text["msg_bits_mp.inc"]]
    extraction = [{"source": "engine/src/qcommon/msg_mp.h", "definition": "msg_t", "line": line,
                   "body_sha256": hashlib.sha256(layout.encode("utf8")).hexdigest()},
                  {"source": "engine/src/qcommon/msg_bits_mp.inc", "definition": "complete native bit primitive include",
                   "line": 1, "body_sha256": hashlib.sha256(source_text["msg_bits_mp.inc"].encode("utf8")).hexdigest()}]
    definitions = [("msg_mp.cpp", name) for name in (
        "MSG_InitReadOnly", "MSG_InitReadOnlySplit", "MSG_BeginReading", "MSG_WriteBit0", "MSG_WriteBit1",
        "MSG_ReadBit", "MSG_ClearLastReferencedEntity", "MSG_ReadEntityIndex")]
    definitions.append(("sv_msg_write_mp.cpp", "MSG_WriteEntityIndex"))
    for filename, name in definitions:
        body, line = extract_function(source_text[filename], name)
        parts.append(body)
        extraction.append({"source": "engine/src/qcommon/" + filename, "definition": name, "line": line,
                           "body_sha256": hashlib.sha256(body.encode("utf8")).hexdigest()})
    generated = CONTRACT / "generated/index_reference_extracted.inc"
    generated.parent.mkdir(parents=True, exist_ok=True)
    generated.write_text("\n\n".join(parts) + "\n", encoding="utf8")
    return source_files, generated, extraction


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate-only", action="store_true", help="No compiler, tests, or game; prepare extracted fixture")
    args = parser.parse_args()
    evidence = STAGE / "results" / datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    evidence.mkdir(parents=True)
    commands = []
    result = {"completed": False, "passed": False,
              "scope": "actual native Win32 message layout and reference-index bodies with external diagnostics stubbed",
              "native_loaded_map_attestation": False, "native_archive_oracle_attestation": False,
              "full_world_replay_attestation": False, "production_applied": False,
              "generate_only": args.generate_only, "commands": commands}
    report = evidence / "result.json"

    def execute(name, argv):
        start = time.perf_counter()
        process = subprocess.run(argv, cwd=STAGE, capture_output=True, text=True, errors="replace")
        log = evidence / (name + ".log")
        log.write_text(process.stdout + process.stderr, encoding="utf8")
        commands.append({"name": name, "argv": [str(arg) for arg in argv], "cwd": str(STAGE),
                         "exit_code": process.returncode, "elapsed_seconds": time.perf_counter() - start,
                         "log": str(log), "log_sha256": sha(log)})
        report.write_text(json.dumps(result, indent=2), encoding="utf8")
        print(f"{name}: exit={process.returncode}; log={log}", flush=True)
        if process.returncode:
            raise RuntimeError(f"{name} failed")

    try:
        probe, proposal = verify_proposal()
        native_sources, generated, extracted = generate()
        inputs = native_sources + [PROJECT / "native/msg_bits.hpp", STAGE / "index_reference_fixture_prefix.inc",
                    STAGE / "index_reference_tests.cpp", Path(__file__).resolve(), CONTRACT / "CMakeLists.txt",
                    STAGE / "prepare_reference_fix.py", probe]
        before = {str(path.relative_to(PROJECT)): sha(path) for path in inputs}
        result.update(source_sha256=before, extraction=extracted, diagnostic_proposal=proposal,
                      generated_sha256={str(generated.relative_to(PROJECT)): sha(generated)})
        if not args.generate_only:
            instance = "C:/BuildTools"
            for line in (PROJECT / "build/Release/CMakeCache.txt").read_text(encoding="utf8").splitlines():
                if line.startswith("CMAKE_GENERATOR_INSTANCE:INTERNAL="):
                    instance = line.split("=", 1)[1]
                    break
            result["toolchain_instance"] = instance
            build = CONTRACT / "build"
            execute("configure", ["cmake", "-S", str(CONTRACT), "-B", str(build), "-G", "Visual Studio 17 2022",
                                  "-A", "Win32", "-DCMAKE_GENERATOR_INSTANCE=" + instance])
            targets = ["archive_index_reference_legacy", "archive_index_reference_packed"]
            execute("build", ["cmake", "--build", str(build), "--config", "Release", "--target", *targets, "--parallel", "1"])
            result["binary_sha256"] = {}
            for target in targets:
                binary = build / "Release" / (target + ".exe")
                result["binary_sha256"][str(binary.relative_to(PROJECT))] = sha(binary)
                execute(target, [str(binary)])
        result["source_stable"] = before == {str(path.relative_to(PROJECT)): sha(path) for path in inputs}
        if not result["source_stable"]:
            raise RuntimeError("A source changed during native reference-index contracts")
        result.update(completed=True, passed=True, contracts_executed=not args.generate_only)
    except Exception as error:
        result.update(completed=True, passed=False, error=str(error))
        raise
    finally:
        report.write_text(json.dumps(result, indent=2), encoding="utf8")
        print("evidence=" + str(report), flush=True)


if __name__ == "__main__":
    main()
