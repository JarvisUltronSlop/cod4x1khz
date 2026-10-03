"""STAGED, unlaunched: authenticated owned-host native archive codec fixture.

One fresh stock Killhouse/Regular/DM/15-bot fixture, normally under90 seconds.
The deliberate synchronous codec stall is NOT a timing sample. Native cleanup,
unchanged-world field oracle and resumed real network/pose/GPU are required.
Run only after root applies/links/archives the reviewed integration and grants an
exclusive game slot. This file never builds or applies source.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import time

STAGE = Path(__file__).resolve().parent
PROJECT = STAGE.parent.parent
sys.path.insert(0, str(PROJECT / "scripts"))
from native_checkpoint_capture_probe import ManagedHostFixture, integer, require_no_errors

BOOLS = ("completed", "passed", "cleanup_complete", "native_codec_executed", "full_delta",
         "live_serialization_oracle", "logical_delay", "real_msg_overflow", "phantom_rejected",
         "descriptor_headroom", "client_64_row_headroom", "split_byte_ring", "byte_eviction",
         "observed_world_unchanged")


def reply_errors(value, before, server, client_pid):
    if type(value) is not dict or type(value.get("origin")) is not dict or type(value.get("codec")) is not dict:
        return ["Missing typed owned-command origin or native codec result"]
    origin, codec = value["origin"], value["codec"]
    errors = []
    owner = before["stats"]["client_owner"]
    if origin.get("direct_owned_command") is not True or origin.get("server_pid") != server.pid or origin.get("parent_pid") != client_pid:
        errors.append("Codec invocation is not the exact created parent/owned child direct command")
    if not integer(origin.get("request_sequence"), 1):
        errors.append("Authenticated host command has no actual request sequence")
    if not integer(value.get("native_owner_wall_ms"), 0):
        errors.append("Missing caller-visible synchronous native diagnostic wall time")
    if origin.get("session") != owner.get("session") or origin.get("session") != codec.get("session"):
        errors.append("Accepted client/native codec/owned command session differ")
    if origin.get("tick") != codec.get("tick") or origin.get("server_id") != codec.get("server_id") or codec.get("server_id") != before["state"].get("server_id"):
        errors.append("Codec does not retain its actual unchanged owned tick/map identity")
    for name in BOOLS:
        if codec.get(name) is not True:
            errors.append("Native codec condition failed: " + name)
    if codec.get("aborted") is not False or codec.get("reason") != "":
        errors.append("Probe refused/aborted or retained a native failure reason")
    if codec.get("full_world_restore") is not False or codec.get("authoritative_replay") is not False:
        errors.append("Codec-only fixture incorrectly attests full world restore/replay")
    if codec.get("clients") != 16 or not integer(codec.get("entities"), 1, 1023):
        errors.append("Native codec did not encode the expected real16-player/linked-entity world")
    for name in ("native_decodes", "serialized_comparisons", "float_comparisons", "full_records", "delta_records", "allocation_bytes", "profile_bytes", "hashed_bytes", "native_time", "tick", "reused_id"):
        if not integer(codec.get(name), 1):
            errors.append("Missing meaningful actual native codec count: " + name)
    if codec.get("logical_cases") != 150 or not integer(codec.get("split_records"), 256):
        errors.append("All actual logical-delay or32MiB split placement cases did not execute")
    first, second = (codec.get("diagnostic_fnv64_before"), codec.get("diagnostic_fnv64_after"))
    if type(first) is not str or len(first) != 16 or any(c not in "0123456789abcdef" for c in first) or first != second:
        errors.append("Exact codec scope restore diagnostic hashes differ/malformed")
    # Native passed includes exact memcmp of every saved member and profile,
    # not just equality of the noncryptographic hashes checked above.
    return errors


def record_build(fixture, record):
    value = json.loads(record.read_text(encoding="utf-8-sig"))
    if not integer(value.get("build"), 1) or value.get("tests", {}).get("passed") is not True:
        raise ValueError("Explicit immutable native build record is invalid")
    expected = value.get("binary_sha256", {})
    for path, key in (("cod4x1khz.exe", "client"), ("runtime/cod4x1khz_server.exe", "server")):
        if fixture.report["binary_sha256"][key] != expected.get(path):
            raise ValueError("Created fixture binary differs from archived native build: " + path)
    fixture.report["build_record"] = {"path": str(record), "build": value["build"],
        "sha256": hashlib.sha256(record.read_bytes()).hexdigest(), "binary_sha256": expected}


def queued_recovery(sample):
    stats = sample["stats"]
    owner, raw = stats["client_owner"], stats["raw_input"]
    return {"elapsed": sample["elapsed"], "frontend_debt": stats.get("debt"),
            "raw_input_queued": raw.get("queued"), "raw_input_dropped": raw.get("dropped"),
            "snapshot_backlog": owner.get("snapshot_backlog"), "event_backlog": owner.get("event_backlog"),
            "maximum_owner_tick_debt": owner.get("maximum_tick_debt"),
            "server_commands_published": owner.get("server_commands_published"),
            "server_commands_presented": owner.get("server_commands_presented")}


def rcon_refusal(fixture):
    # A public RCON command must never inherit the owned-pipe execution scope.
    # Actual endpoint PID is checked separately by native_server immediately
    # before/after; retain the exact bounded native datagram rather than treating
    # a fire-and-forget send as a proved refusal.
    delay = .34 - (time.monotonic() - fixture.last_rcon)
    if delay > 0:
        time.sleep(delay)
    fixture.last_rcon = time.monotonic()
    request = b"\xff" * 4 + b"rcon local-khz-benchmark khz_archive_probe"
    received = bytearray()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(1.2)
        sock.sendto(request, ("127.0.0.1", fixture.server_port))
        while len(received) < 8192:
            data, address = sock.recvfrom(8192)
            if address != ("127.0.0.1", fixture.server_port):
                raise ValueError("Public RCON refusal arrived from a different endpoint")
            received.extend(data)
            if b"KHZ_ARCHIVE_PROBE_REFUSED authenticated_direct_owned_command_required" in received:
                break
    if b"KHZ_ARCHIVE_PROBE {" in received or b"KHZ_ARCHIVE_PROBE_REFUSED authenticated_direct_owned_command_required" not in received:
        raise ValueError("Public RCON codec execution was not refused by the actual native origin guard")
    (fixture.output / "rcon-refusal.datagram").write_bytes(received)
    return {"passed": True, "bytes": len(received), "sha256": hashlib.sha256(received).hexdigest(),
            "origin": "ordinary authenticated RCON, without owned pipe Command scope"}


def wait_reply(fixture, path, start, timeout):
    until = min(fixture.deadline, time.monotonic() + timeout)
    while time.monotonic() < until:
        if fixture.client.poll() is not None or not fixture.server.alive():
            raise RuntimeError("Exact created client/server exited during native codec probe")
        raw = path.read_bytes()[start:] if path.is_file() else b""
        if b"[output truncated]" in raw or b"KHZ_ARCHIVE_PROBE_REFUSED" in raw:
            (fixture.output / "owned-command-output.txt").write_bytes(raw)
            raise ValueError("Native owned command refused or truncated its codec result")
        lines = [line for line in raw.splitlines() if b"KHZ_ARCHIVE_PROBE {" in line]
        if lines:
            if len(lines) != 1:
                raise ValueError("More than one native codec result for one actual owned command")
            exact = lines[0].split(b"KHZ_ARCHIVE_PROBE ", 1)[1]
            value = json.loads(exact.decode("utf8"))
            (fixture.output / "owned-command-output.txt").write_bytes(raw)
            (fixture.output / "native-codec-reply.json").write_bytes(exact + b"\n")
            return value, {"sha256": hashlib.sha256(exact).hexdigest(), "bytes": len(exact),
                           "source": str(path), "initial_log_offset": start}
        time.sleep(.05)
    raise TimeoutError("Actual owned child did not return a complete native codec reply")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-record", type=Path, required=True)
    parser.add_argument("--client-port", type=int, default=28991)
    parser.add_argument("--budget", type=int, default=90)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if os.name != "nt" or not 1024 <= args.client_port <= 65535 or not 45 <= args.budget <= 150:
        parser.error("Windows, port1024..65535 and budget45..150 are required")
    build_record = args.build_record.resolve()
    if build_record.parent != (PROJECT / "results").resolve() or not build_record.is_file():
        parser.error("Build record must be an existing direct project/results file")
    output = (args.output or PROJECT / "results" / ("native-archive-codec-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))).resolve()
    if output.parent != (PROJECT / "results").resolve() or output.exists() or output.is_symlink() or output.is_junction():
        parser.error("Output must be a new direct project/results child")
    output.mkdir()
    fixture = ManagedHostFixture(PROJECT, output, args.client_port, budget=args.budget, resolution="480p")
    fixture.report.update(schema=1, scope="Actual unchanged loaded-map native archive codec-only oracle/restore; authenticated owned child; no whole-world restore, server replay or timing gate.",
        full_world_restore=False, authoritative_replay=False, timing_gate=False,
        probe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    try:
        record_build(fixture, build_record)
        before = fixture.start()
        fixture.report["before"] = before
        fixture.report["native_server_before"] = fixture.native_server()
        fixture.report["public_rcon_refusal"] = rcon_refusal(fixture)
        fixture.report["native_server_after_refusal"] = fixture.native_server()
        log = fixture.home / "main/khz_client.log"
        if not log.is_file():
            raise ValueError("Fresh exact-client native log is unavailable for owned command reply")
        start = log.stat().st_size
        requested = time.monotonic()
        fixture.report["owned_command_request"] = fixture.call("khz_ui server khz_archive_probe")
        if fixture.report["owned_command_request"].get("result") != "server command queued":
            raise ValueError("Private stock UI/HostConsoleCommand adapter rejected the command")
        reply, provenance = wait_reply(fixture, log, start, 35)
        fixture.report["diagnostic_elapsed_wall_seconds"] = time.monotonic() - requested
        fixture.report["codec_reply"], fixture.report["codec_reply_provenance"] = reply, provenance
        require_no_errors(reply_errors(reply, before, fixture.server, fixture.client.pid))
        fixture.report["after"] = fixture.wait_ready("post-codec restored native gameplay", before, timeout=20,
                                                     require_bounded_debt=True)
        fixture.report["native_server_after"] = fixture.native_server()
        recovery = fixture.report["readiness"][-1]
        fixture.report["queued_recovery_diagnostics"] = {
            "native_owner_wall_ms": reply["native_owner_wall_ms"],
            "queue_to_reply_wall_seconds": fixture.report["diagnostic_elapsed_wall_seconds"],
            "client_samples": [queued_recovery(sample) for sample in recovery["samples"]],
            "client_debt": recovery["client_debt"], "server_debt": recovery["server_debt"],
            "server_samples": [{"elapsed": sample["elapsed"], "debt": sample["stats"].get("debt"),
                                "commands_received": sample["stats"].get("commands_received"),
                                "snapshots_sent": sample["stats"].get("snapshots_sent")}
                               for sample in recovery["server_samples"]],
            "timing_acceptance_sample": False}
        fixture.leave()
        fixture.report["passed"] = True
    except Exception as error:
        fixture.report["errors"].append(str(error))
    finally:
        fixture.close()
    print(json.dumps({"passed": fixture.report["passed"], "output": str(output),
                      "full_world_restore": False, "authoritative_replay": False, "timing_gate": False}, indent=2))
    return 0 if fixture.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
