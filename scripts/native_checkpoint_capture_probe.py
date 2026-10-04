"""Short actual private-client checkpoint and authoritative-input tape diagnostic.

The profile action starts a separate fresh match. Neither action attests full
world replay, renderer equivalence, public-feature parity, or a timing gate.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time

from monitor import query
from native_lifecycle_probe import OwnedServer, continuity, counter_delta, ready_errors, server_continuity, windows_for_pid


NATIVE_DOMAINS = 1023
CANONICAL_DOMAINS = 127
ALL_SLOTS = 65535
TAPE_TICKS = 1000


def integer(value, low=0, high=None):
    return type(value) is int and value >= low and (high is None or value <= high)


def checkpoint_errors(reply, owner):
    errors = []
    if reply.get("result") != "restored" or reply.get("stage") != "complete" or reply.get("cleanup") is not True:
        errors.append(f"private roundtrip was not restored, complete and cleaned up (result={reply.get('result')!r}, stage={reply.get('stage')!r}, reason={reply.get('reason')!r})")
    if reply.get("full_world_replay") is not False:
        errors.append("private roundtrip must explicitly disclaim full-world replay")
    image = reply.get("checkpoint", {})
    expected = {"schema": 1, "required_domains": NATIVE_DOMAINS, "provider_mask": NATIVE_DOMAINS,
                "captured_mask": NATIVE_DOMAINS, "validated_mask": NATIVE_DOMAINS, "restored_mask": NATIVE_DOMAINS,
                "required_canonical_domains": CANONICAL_DOMAINS, "roundtrip_domain_mask": CANONICAL_DOMAINS,
                "failure": 0, "failures": 0}
    for name, value in expected.items():
        if not integer(image.get(name)) or image[name] != value:
            errors.append(f"private checkpoint {name} does not match {value}")
    for name, expected_value in (("pending", False), ("assets_pinned", True), ("complete", True), ("private_restore_complete", True)):
        if image.get(name) is not expected_value:
            errors.append(f"private checkpoint {name} is missing or incorrect")
    for name in ("fixture", "logical_tick", "storage_bytes", "captures", "validations", "restorations"):
        if not integer(image.get(name), 1):
            errors.append(f"private checkpoint lacks actual {name}")
    if not integer(reply.get("fixture"), 1) or reply.get("fixture") != image.get("fixture"):
        errors.append("private checkpoint fixture identity differs")
    if any(not integer(owner.get(name), 1) or image.get(name) != owner[name] for name in ("session", "generation")):
        errors.append("private checkpoint does not retain the actual owner session/generation")
    if image.get("reason") != "":
        errors.append("private checkpoint reported a rejection reason")
    return errors


def tape_status_errors(status, drained=False):
    errors = []
    expected = {"schema": 1, "target_ticks": TAPE_TICKS, "capacity_slots": 16, "recorded": TAPE_TICKS,
                "invalid": 0, "rejected": 0, "verified_restore_conditions": 0,
                "drained": TAPE_TICKS if drained else 0, "queued": 0 if drained else TAPE_TICKS}
    for name, value in expected.items():
        if not integer(status.get(name)) or status[name] != value:
            errors.append(f"authoritative tape {name} does not match {value}")
    for name, value in (("active", False), ("tick_open", False), ("finished", True), ("complete", True),
                        ("export_complete", drained), ("full_world_replay_verified", False)):
        if status.get(name) is not value:
            errors.append(f"authoritative tape {name} is missing or incorrect")
    for name in ("fixture", "session", "storage_bytes", "first_tick", "last_tick"):
        if not integer(status.get(name), 1):
            errors.append(f"authoritative tape lacks {name}")
    if not integer(status.get("capacity_ticks"), TAPE_TICKS) or not integer(status.get("server_id"), 1):
        errors.append("authoritative tape capacity or native server identity is missing")
    if not integer(status.get("first_tick"), 1) or status.get("last_tick") != status["first_tick"] + TAPE_TICKS - 1:
        errors.append("authoritative tape tick endpoints are not consecutive")
    if status.get("failure") != "none":
        errors.append("authoritative tape reported a native failure")
    return errors


def tape_record_errors(record, status, ordinal):
    errors = []
    if not integer(record.get("schema")) or record["schema"] != 1 or record.get("found") is not True or record.get("complete") is not True:
        errors.append("authoritative tick record is missing or incomplete")
    for name in ("fixture", "session", "server_id"):
        if record.get(name) != status.get(name) or not integer(record.get(name), 1):
            errors.append(f"authoritative record {name} differs from the captured tape")
    first_tick = status.get("first_tick")
    if not integer(record.get("ordinal"), 1) or record["ordinal"] != ordinal or not integer(first_tick, 1) or record.get("tick") != first_tick + ordinal - 1:
        errors.append("authoritative record ordinal/tick differs")
    if not integer(record.get("server_time"), -2147483648, 2147483647):
        errors.append("authoritative record native time is missing")
    for name in ("active_mask_before", "active_mask_after", "recorded_mask"):
        if not integer(record.get(name)) or record[name] != ALL_SLOTS:
            errors.append(f"authoritative record {name} does not include all 16 actual slots")
    if not integer(record.get("verified_restore_conditions")) or record["verified_restore_conditions"] != 0 or record.get("failure") != "none":
        errors.append("authoritative record failed or falsely attested restoration")
    slots = record.get("slots")
    if type(slots) is not list or len(slots) != 16:
        return errors + ["authoritative record does not contain exactly 16 native slot records"]
    if any(type(slot) is not dict or not integer(slot.get("slot"), 0, 15) for slot in slots) or sorted(slot["slot"] for slot in slots) != list(range(16)):
        return errors + ["authoritative slot set is duplicated or incomplete"]
    if sum(slot.get("source") == "native_bot" for slot in slots) != 15 or sum(slot.get("source") in ("queued", "held") for slot in slots) != 1:
        errors.append("authoritative sources do not contain 15 native bots and one real human")
    for slot in slots:
        if not integer(slot.get("generation"), 1):
            errors.append(f"native slot {slot['slot']} has no admission generation")
        for name in ("input_sequence_before", "input_sequence_after", "applied_sequence", "received_sequence", "view_tick", "consumed_commands"):
            if not integer(slot.get(name)):
                errors.append(f"native slot {slot['slot']} lacks input metadata {name}")
        if type(slot.get("input_advanced")) is not bool:
            errors.append(f"native slot {slot['slot']} lacks an input advancement flag")
        for name in ("entity_use_count", "last_connect_time", "last_spawn_time", "server_id"):
            if not integer(slot.get(name), -2147483648, 2147483647):
                errors.append(f"native slot {slot['slot']} lacks actual native {name}")
        if not integer(slot.get("script_id"), 0, 65535):
            errors.append(f"native slot {slot['slot']} lacks a bounded native script identity")
        command = slot.get("command", {})
        for name in ("server_time", "buttons"):
            if not integer(command.get(name), -2147483648, 2147483647):
                errors.append(f"native slot {slot['slot']} has invalid command {name}")
        for name in ("weapon", "offhand", "melee_distance"):
            if not integer(command.get(name), 0, 255):
                errors.append(f"native slot {slot['slot']} has invalid command {name}")
        for name in ("forward", "right"):
            if not integer(command.get(name), -128, 127):
                errors.append(f"native slot {slot['slot']} has invalid movement byte {name}")
        for name, length, low, high in (("angles", 3, -2147483648, 2147483647), ("selected_location", 2, -128, 127)):
            values = command.get(name)
            if type(values) is not list or len(values) != length or any(not integer(value, low, high) for value in values):
                errors.append(f"native slot {slot['slot']} has invalid command array {name}")
        yaw = command.get("melee_yaw")
        if type(yaw) not in (int, float) or not math.isfinite(yaw):
            errors.append(f"native slot {slot['slot']} has invalid melee yaw")
        if command.get("server_time") != record.get("server_time"):
            errors.append(f"native slot {slot['slot']} command does not use this authoritative millisecond")
        if integer(record.get("tick"), 1) and all(integer(slot.get(name)) for name in ("input_sequence_before", "input_sequence_after", "consumed_commands", "view_tick")):
            advanced = slot["input_sequence_after"] > slot["input_sequence_before"]
            if slot["input_sequence_after"] < slot["input_sequence_before"] or slot.get("input_advanced") is not advanced or \
                    slot["consumed_commands"] > 2048 or slot["view_tick"] > record["tick"]:
                errors.append(f"native slot {slot['slot']} has inconsistent native latch metadata")
            if slot.get("source") == "native_bot":
                if slot["consumed_commands"] != 0 or slot["input_sequence_before"] != record["tick"] or slot["input_sequence_after"] != record["tick"]:
                    errors.append(f"native bot slot {slot['slot']} does not identify this actual tick")
            elif all(integer(slot.get(name)) for name in ("applied_sequence", "received_sequence")):
                if slot["applied_sequence"] > slot["received_sequence"] or slot["input_sequence_after"] > slot["received_sequence"] or \
                        (slot.get("source") == "held") != (slot["consumed_commands"] == 0) or \
                        (slot.get("source") == "held" and slot["input_sequence_after"] != slot["input_sequence_before"]):
                    errors.append(f"native human slot {slot['slot']} has inconsistent receipt/selection metadata")
    return errors


def record_pair_errors(first, last):
    errors = []
    if last.get("server_time") != first.get("server_time", 0) + TAPE_TICKS - 1:
        errors.append("native authoritative record millisecond endpoints are not consecutive")
    a = {slot["slot"]: (slot["generation"], slot["source"] == "native_bot") for slot in first.get("slots", [])}
    b = {slot["slot"]: (slot["generation"], slot["source"] == "native_bot") for slot in last.get("slots", [])}
    if a != b:
        errors.append("native slot admissions or bot/human identities changed during the tape")
    return errors


def tape_export_errors(metadata, status):
    errors = []
    if metadata.get("committed") is not True or metadata.get("failure") != "none":
        errors.append("Native authoritative tape file was not atomically committed")
    expected = {"schema": 1, "records": TAPE_TICKS, **{name: status.get(name) for name in
                ("fixture", "session", "server_id", "first_tick", "last_tick")}}
    for name, value in expected.items():
        if not integer(metadata.get(name)) or metadata[name] != value:
            errors.append(f"authoritative export {name} does not match the retained tape")
    if metadata.get("file") != "main/khz_server_capture.jsonl" or metadata.get("format") != "jsonl":
        errors.append("authoritative export is not the fixed benchmark JSONL artifact")
    if not integer(metadata.get("bytes"), 1, 16384 * TAPE_TICKS) or not integer(metadata.get("elapsed_us")):
        errors.append("authoritative export byte count or measured diagnostic stall is missing")
    digest = metadata.get("sha256")
    if type(digest) is not str or len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
        errors.append("authoritative export has no exact SHA256 digest")
    if metadata.get("full_world_replay_verified") is not False:
        errors.append("authoritative export must explicitly disclaim full-world restoration")
    return errors


def validate_tape_jsonl(data, status, first, last):
    """Verify every actual serialized record, never infer contents from drain."""
    if not data or len(data) > 16384 * TAPE_TICKS or not data.endswith(b"\n"):
        raise ValueError("Authoritative JSONL file is empty, oversized or unterminated")
    lines = data.splitlines()
    if len(lines) != TAPE_TICKS:
        raise ValueError("Authoritative JSONL does not retain exactly 1,000 records")
    previous = None
    for ordinal, line in enumerate(lines, 1):
        if not line or len(line) >= 16384:
            raise ValueError(f"Authoritative JSONL record {ordinal} is empty or exceeds native codec bounds")
        record = json.loads(line)
        require_no_errors(tape_record_errors(record, status, ordinal))
        if previous is not None:
            if record["tick"] != previous["tick"] + 1 or record["server_time"] != previous["server_time"] + 1:
                raise ValueError(f"Authoritative JSONL record {ordinal} breaks actual tick/millisecond continuity")
            before = {slot["slot"]: slot for slot in previous["slots"]}
            for slot in record["slots"]:
                old = before[slot["slot"]]
                if slot["generation"] != old["generation"] or (slot["source"] == "native_bot") != (old["source"] == "native_bot"):
                    raise ValueError(f"Authoritative JSONL slot admission changed at record {ordinal}")
                if slot["input_sequence_after"] < old["input_sequence_after"]:
                    raise ValueError(f"Authoritative JSONL native input sequence decreased at record {ordinal}")
        if (ordinal == 1 and record != first) or (ordinal == TAPE_TICKS and record != last):
            raise ValueError("Authoritative JSONL endpoints differ from independently read native records")
        previous = record
    return {"records_validated": TAPE_TICKS, "slot_records_validated": TAPE_TICKS * 16,
            "all_records_retained": True, "full_world_replay_verified": False}


def retain_tape_export(fixture, tape):
    metadata = tape["export"] = fixture.rcon("khz_server_capture export", "KHZ_SERVER_CAPTURE_EXPORT", 10)
    require_no_errors(tape_export_errors(metadata, tape["status"]))
    source = fixture.server_home / "main/khz_server_capture.jsonl"
    if any(path.is_symlink() for path in (fixture.server_home, source.parent, source)) or \
            source.resolve().parent != (fixture.server_home / "main").resolve() or not source.is_file():
        raise ValueError("Authoritative export is not the exact owned benchmark-home artifact")
    with source.open("rb") as file:
        data = file.read(16384 * TAPE_TICKS + 1)
    if len(data) != metadata["bytes"] or hashlib.sha256(data).hexdigest() != metadata["sha256"]:
        raise ValueError("Authoritative export length/hash differs from the committed native file")
    destination = fixture.output / "authoritative-input-tape.jsonl"
    destination.write_bytes(data)
    tape["retained_file"] = {"path": str(destination), "bytes": len(data), "sha256": metadata["sha256"]}
    tape.update(validate_tape_jsonl(data, tape["status"], tape["first"], tape["last"]))
    tape["scope"] = "Every actual serialized authoritative input record validated and retained; no server world/script/RNG restoration or full-world replay"


def export_artifact_identity(fixture):
    source = fixture.server_home / "main/khz_server_capture.jsonl"
    if any(path.is_symlink() or path.is_junction() for path in (fixture.server_home, source.parent, source)):
        raise ValueError("Native export fixture artifact path is a reparse point")
    if not source.exists():
        return None
    if not source.is_file() or source.stat().st_size > 16384 * 4096:
        raise ValueError("Prior native export artifact is not a bounded regular file")
    return {"bytes": source.stat().st_size, "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}


def reject_empty_tape_export(fixture, tape):
    cases = tape.setdefault("export_failure_cases", {})
    before = cases["prior_artifact_before_empty"] = export_artifact_identity(fixture)
    ledger = cases["ledger_before_empty"] = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
    reply = cases["empty_tape"] = fixture.rcon("khz_server_capture export", "KHZ_SERVER_CAPTURE_EXPORT", 3)
    if reply.get("committed") is not False or reply.get("failure") != "requires_complete_current_immutable_tape" or \
            reply.get("records") != 0 or reply.get("bytes") != 0 or reply.get("full_world_replay_verified") is not False:
        raise ValueError("Native exporter did not reject an actual empty tape")
    after = cases["prior_artifact_after_empty"] = export_artifact_identity(fixture)
    if after != before:
        raise ValueError("Rejected native empty-tape export modified an existing artifact")
    cases["ledger_after_empty"] = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
    if cases["ledger_after_empty"] != ledger:
        raise ValueError("Rejected native empty-tape export modified its actual tape ledger")
    cases["empty_tape_preserved_prior_artifact"] = True


def reject_owned_temporary_collision(fixture, tape):
    cases = tape.setdefault("export_failure_cases", {})
    before = cases["prior_artifact_before_collision"] = export_artifact_identity(fixture)
    temporary = fixture.server_home / "main" / f"khz_server_capture.{fixture.server.pid}.{tape['status']['fixture']}.tmp"
    sentinel = b"owned native export collision fixture\n"
    owned = False
    try:
        with temporary.open("xb") as file:
            file.write(sentinel)
        owned = True
        cases["owned_temporary_sentinel"] = {"path": str(temporary), "bytes": len(sentinel), "sha256": hashlib.sha256(sentinel).hexdigest()}
        reply = cases["temporary_collision"] = fixture.rcon("khz_server_capture export", "KHZ_SERVER_CAPTURE_EXPORT", 3)
        if reply.get("committed") is not False or reply.get("failure") != "temporary_create" or \
                reply.get("records") != 0 or reply.get("bytes") != 0 or reply.get("full_world_replay_verified") is not False:
            raise ValueError("Native exporter did not reject its existing exact temp path")
        if not temporary.is_file() or temporary.read_bytes() != sentinel:
            raise ValueError("Native rejected export deleted or changed the unowned temp sentinel")
        after = cases["prior_artifact_after_collision"] = export_artifact_identity(fixture)
        if after != before:
            raise ValueError("Native rejected temp collision modified the prior committed artifact")
        cases["temporary_collision_preserved_files"] = True
    finally:
        if owned and temporary.is_file() and not temporary.is_symlink() and temporary.read_bytes() == sentinel:
            temporary.unlink()
            cases["owned_temporary_removed"] = True
    status = cases["status_after_rejections"] = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
    require_no_errors(tape_status_errors(status))
    if any(status.get(name) != tape["status"].get(name) for name in
           ("fixture", "session", "server_id", "first_tick", "last_tick", "recorded", "drained", "queued")):
        raise ValueError("Native rejected export modified the actual tape ledger or identity")
    cases["rejections_preserved_tape_ledger"] = True


def require_no_errors(errors):
    if errors:
        raise ValueError("; ".join(errors))


def debt_bounds(samples):
    values = [sample.get("debt") for sample in samples]
    if len(values) < 2 or any(not integer(value) for value in values):
        raise ValueError("Actual scheduler debt samples are missing or invalid")
    result = {"start_debt": values[0], "end_debt": values[-1], "max_sampled_debt": max(values),
              "debt_pass": values[-1] <= max(8, values[0] + 2)}
    if not result["debt_pass"]:
        raise ValueError("Actual scheduler debt grew after the off-tick diagnostic export")
    return result


def server_ready_errors(result, client_port):
    peers = [peer for peer in result.get("peers", []) if peer.get("port") == client_port]
    bots = result.get("native_bots", {})
    if len(peers) != 1 or peers[0].get("commands", 0) <= 0 or peers[0].get("snapshots", 0) <= 0 or bots.get("active") != 15 or bots.get("humans") != 1 or bots.get("difficulty") != 1:
        return ["Actual native server does not yet report one active human and 15 Regular bots"]
    return []


def leave_frontend_complete(stats, state, ui, child_exited):
    presentation = stats.get("presentation_state", {})
    return child_exited is True and ui.get("host", {}).get("owned") is False and \
        type(state.get("connection")) is int and state["connection"] == 0 and \
        ui.get("top_menu") in ("main", "main_text") and \
        presentation.get("renderer_started") is True and presentation.get("screen_initialized") is True and \
        presentation.get("engine_error") is False


class ManagedHostFixture:
    """Own one fresh benchmark client and retain the exact spawned server handle."""
    def __init__(self, project, output, port, profile=False, budget=90, retain_all_records=False, resolution="480p", fullscreen=False,
                 client_binary=None, client_settings=None):
        if resolution not in ("480p", "720p"):
            raise ValueError("Native fixture resolution must be 480p or 720p")
        self.project, self.output, self.port = project, output, port
        self.resolution = resolution
        self.fullscreen = bool(fullscreen)
        self.binary, self.server_binary = client_binary or project / "cod4x1khz.exe", project / "runtime/cod4x1khz_server.exe"
        self.client_settings = dict(client_settings or {})
        self.home = (project / "runtime/benchmarks" / f"client-{port}").resolve()
        if self.home.parent != (project / "runtime/benchmarks").resolve() or self.home.is_symlink():
            raise ValueError("Fixture home must be a direct non-symlink benchmark child")
        self.saved_home = output / "previous-client-home"
        self.client = self.server = self.server_home = None
        self.home_authorized = False
        self.started, self.last_rcon = time.monotonic(), float("-inf")
        self.deadline = self.started + budget
        self.profile = profile
        self.retain_all_records = retain_all_records
        self.report = {"passed": False, "errors": [], "client_port": port, "fresh_benchmark_home": str(self.home),
                       "resolution": resolution,
                       "fullscreen_requested": self.fullscreen,
                       "started_utc": datetime.now(timezone.utc).isoformat(), "profile_action": profile,
                       "full_tape_retention_required": retain_all_records,
                       "binary_sha256": {"client": hashlib.sha256(self.binary.read_bytes()).hexdigest(),
                                         "server": hashlib.sha256(self.server_binary.read_bytes()).hexdigest()},
                       "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                       "scope": "Actual same-process private-client checkpoint and actual post-plugin authoritative input tape; "
                                "profile action is a separate fresh native match. No server world/script/RNG restore, full-world replay, "
                                "renderer equivalence, sustained timing or complete public-feature parity attestation.",
                       "full_world_replay_verified": False}

    def call(self, command, marker="KHZ_UI", timeout=1.2):
        if self.client is None or self.client.poll() is not None:
            raise RuntimeError("Created fixture client is not alive")
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Native fixture 90-second diagnostic budget exhausted")
        return query(self.port, command, marker, min(timeout, remaining))

    def sample(self):
        stats = self.call("khz_stats", "KHZ_JSON")
        if stats.get("process_id") != self.client.pid:
            raise RuntimeError("Fixture endpoint differs from the created process")
        return {"elapsed": time.monotonic() - self.started, "stats": stats,
                "state": self.call("khz_state", "KHZ_STATE"), "ui": self.call("khz_ui status")}

    def rcon(self, command, marker="KHZ_JSON", timeout=1.5, fire=False):
        if self.server is None or not self.server.alive():
            raise RuntimeError("Exact owned server is not alive")
        delay = self.last_rcon + 1.1 - time.monotonic()
        if delay > 0:
            if time.monotonic() + delay >= self.deadline:
                raise TimeoutError("Fixture budget exhausted before paced RCON")
            time.sleep(delay)
        self.last_rcon = time.monotonic()
        text = "rcon local-khz-benchmark " + command
        if fire:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.sendto(b"\xff" * 4 + text.encode("ascii"), ("127.0.0.1", self.server_port))
            return None
        return query(self.server_port, text, marker, min(timeout, max(.01, self.deadline - time.monotonic())))

    def native_server(self, require_ready=True):
        result = self.rcon("khz_json")
        if result.get("process_id") != self.server.pid:
            raise RuntimeError("RCON endpoint is not the exact owned child")
        if require_ready:
            require_no_errors(server_ready_errors(result, self.port))
        if result.get("plugins", {}).get("loaded") != 0:
            raise RuntimeError("Checkpoint fixture cannot contain a loaded plugin")
        return result

    def wait_ready(self, label, baseline=None, timeout=30, require_bounded_debt=False):
        entry = {"name": label, "passed": False, "samples": []}
        self.report.setdefault("readiness", []).append(entry)
        until = min(self.deadline, time.monotonic() + timeout)
        first = first_server = None

        def fresh_server():
            value = self.native_server(require_ready=False)
            entry.setdefault("server_samples", []).append({"elapsed": time.monotonic() - self.started, "stats": value})
            entry["server"] = value
            require_no_errors(server_ready_errors(value, self.port))
            counter_delta(value, value, "time_rejected")
            return value

        while time.monotonic() < until:
            if self.server and not self.server.alive():
                raise RuntimeError("Exact owned server exited during the fixture")
            try:
                current = self.sample(); entry["samples"].append(current)
                host = current["ui"].get("host", {})
                if self.server is None and host.get("owned") is True and host.get("authenticated") is True and integer(host.get("child_pid"), 1):
                    self.server = OwnedServer(host["child_pid"], self.server_binary)
                    self.server_port = host["port"]
                    self.server_home = self.project / "runtime/benchmarks" / f"server-{self.server_port}"
                    self.report["owned_server"] = {"pid": self.server.pid, "path": self.server.path, "port": self.server_port}
                errors = ready_errors(current, "mp_killhouse", self.client.pid, self.server.pid if self.server else None)
                if baseline and any(current["stats"].get("client_owner", {}).get(name) != baseline["stats"]["client_owner"][name] for name in ("session", "generation")):
                    errors.append("Private checkpoint changed the actual owner lifetime")
                entry["last_errors"] = errors
                if not errors:
                    if first and current["elapsed"] - first["elapsed"] >= .3:
                        entry["continuity"] = continuity(first, current)
                        if baseline:
                            entry["continuity_since_checkpoint"] = continuity(baseline, current)
                        latest_server = fresh_server()
                        entry["server_continuity"] = server_continuity(first_server, latest_server, self.port)
                        if require_bounded_debt:
                            entry["client_debt"] = debt_bounds([sample["stats"] for sample in entry["samples"]])
                            entry["server_debt"] = debt_bounds([sample["stats"] for sample in entry["server_samples"]])
                        entry["windows"] = windows_for_pid(self.client.pid)
                        if not any(window["visible"] and not window["minimized"] for window in entry["windows"]):
                            raise RuntimeError("Fixture client has no visible native window")
                        entry.update(passed=True, recovered=current)
                        return current
                    if first is None:
                        first_server = fresh_server()
                        first = current
                else:
                    first = first_server = None
            except ValueError as error:
                entry["last_errors"] = [str(error)]; first = first_server = None
            except (TimeoutError, OSError) as error:
                entry["last_query_error"] = str(error); first = first_server = None
            time.sleep(.1)
        raise TimeoutError(f"Native {label} readiness failed: {entry.get('last_errors')}; {entry.get('last_query_error', '')}")

    def start(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", self.port))
        if self.home.exists():
            self.home.rename(self.saved_home)
            self.report["previous_home_preserved"] = True
        self.home_authorized = True
        dimensions = {"480p": "640x480", "720p": "1280x720"}[self.resolution]
        arguments = [str(self.binary), "allowdupe", "+set", "net_port", str(self.port), "+set", "khz_benchmark", "1",
                     "+set", "khz_test_controls", "1", "+set", "com_introPlayed", "1", "+set", "r_mode", dimensions,
                     "+set", "r_customMode", dimensions, "+set", "r_fullscreen", "1" if self.fullscreen else "0"]
        cfg = next((path for path in (self.project / "runtime/main/client.cfg", self.project / "runtime/client.cfg") if path.is_file()), None)
        if cfg:
            arguments += ["+exec", "client.cfg"]
            self.report["benchmark_cfg"] = {"path": str(cfg), "sha256": hashlib.sha256(cfg.read_bytes()).hexdigest()}
        arguments += ["+set", "com_maxfps", "0", "+set", "r_vsync", "0", "+setqport", str(self.port)]
        arguments += ["+set", "developer", "1" if self.profile else "0",
                      "+set", "khz_profile_scripts", "1" if self.profile else "0"]
        for name, value in self.client_settings.items():
            arguments += ["+set", name, str(value)]
        self.report["launch"] = {"arguments": arguments, "cwd": r"C:\Windows\Temp", "auto_connect": False}
        self.client = subprocess.Popen(subprocess.list2cmdline(arguments), cwd=r"C:\Windows\Temp")
        self.report["owned_client_pid"] = self.client.pid
        until = min(self.deadline, time.monotonic() + 30)
        samples = self.report["startup_samples"] = []
        while time.monotonic() < until:
            try:
                current = self.sample(); samples.append(current)
                if current["state"].get("connection") == 0 and current["ui"].get("top_menu") in ("main", "main_text"):
                    if current["ui"].get("host", {}).get("owned") is not False:
                        raise RuntimeError("No-auto-connect startup already owns a server")
                    self.report["initial"] = current
                    break
            except (TimeoutError, OSError) as error:
                self.report["last_startup_error"] = str(error)
            time.sleep(.1)
        if "initial" not in self.report:
            raise TimeoutError("Fresh fixture did not reach the stock disconnected main menu")
        self.report["host_request"] = self.call("khz_ui host mp_killhouse dm 15 1")
        if self.report["host_request"].get("result") != "start requested":
            raise RuntimeError("Native owned host request was rejected")
        self.wait_ready("initial host")
        time.sleep(2)
        return self.wait_ready("warm host")

    def leave(self):
        self.report["leave_request"] = self.call("khz_ui leave")
        if self.report["leave_request"].get("result") != "queued":
            raise RuntimeError("Explicit native Leave was rejected")
        until = min(self.deadline, time.monotonic() + 10)
        samples = self.report["leave_samples"] = []
        while time.monotonic() < until:
            try:
                sample = self.sample(); samples.append(sample)
                if leave_frontend_complete(sample["stats"], sample["state"], sample["ui"], not self.server.alive()):
                    self.report["explicit_leave_stopped_exact_server"] = True
                    return
            except (TimeoutError, OSError) as error:
                # Native Leave reloads UI assets before serving its next OOB
                # diagnostic. Retain the gap, then require the actual frontend.
                self.report["last_leave_query_error"] = str(error)
            time.sleep(.1)
        raise TimeoutError("Native Leave did not stop the exact child and disconnect")

    def close(self):
        cleanup = self.report["cleanup"] = {}
        if self.client and self.client.poll() is None:
            try:
                stats = query(self.port, "khz_stats", "KHZ_JSON", 1)
                if stats.get("process_id") != self.client.pid:
                    raise ValueError("Cleanup diagnostic endpoint is not the created client")
                if not self.report.get("explicit_leave_stopped_exact_server"):
                    cleanup["leave"] = query(self.port, "khz_ui leave", "KHZ_UI", 1)
                    # Leave can unload the map and reload stock UI assets.
                    # Queue Quit only after that native frontend is ready.
                    until = time.monotonic() + 10
                    samples = cleanup["leave_samples"] = []
                    while self.client.poll() is None and time.monotonic() < until:
                        try:
                            def bounded_query(command, marker):
                                remaining = until - time.monotonic()
                                if remaining <= 0:
                                    raise TimeoutError("Cleanup Leave/frontend readiness budget exhausted")
                                return query(self.port, command, marker, min(1, remaining))
                            current_stats = bounded_query("khz_stats", "KHZ_JSON")
                            if current_stats.get("process_id") != self.client.pid:
                                raise ValueError("Cleanup readiness endpoint is not the created client")
                            state = bounded_query("khz_state", "KHZ_STATE")
                            ui = bounded_query("khz_ui status", "KHZ_UI")
                            samples.append({"elapsed": time.monotonic() - self.started,
                                            "presentation_state": current_stats.get("presentation_state"),
                                            "connection": state.get("connection"), "top_menu": ui.get("top_menu"), "host": ui.get("host")})
                            if leave_frontend_complete(current_stats, state, ui, self.server is None or not self.server.alive()):
                                cleanup["leave_frontend_ready"] = True
                                break
                        except (TimeoutError, OSError) as error:
                            cleanup["last_leave_query_error"] = str(error)
                        time.sleep(min(.1, max(0, until - time.monotonic())))
                    if self.client.poll() is None and not cleanup.get("leave_frontend_ready"):
                        raise TimeoutError("Native Leave did not finish stock frontend reload before cleanup Quit")
                if self.client.poll() is None:
                    cleanup["quit"] = query(self.port, "khz_ui quit", "KHZ_UI", 1)
            except (TimeoutError, OSError, ValueError) as error:
                cleanup["query_error"] = str(error)
            try:
                self.client.wait(timeout=8)
            except subprocess.TimeoutExpired:
                cleanup["client_forced_stop"] = True
                self.client.terminate(); self.client.wait(timeout=5)
        cleanup["created_client_exited"] = self.client is None or self.client.poll() is not None
        cleanup["created_client_returncode"] = self.client.returncode if self.client else None
        if self.client and self.client.returncode:
            self.report["errors"].append(f"Created client exited with {self.client.returncode}")
        if self.server:
            try:
                if self.server.alive():
                    cleanup["server_forced_stop"] = True; self.server.stop()
                cleanup["exact_owned_server_exited"] = not self.server.alive()
            except Exception as error:
                cleanup["exact_owned_server_exited"] = False
                self.report["errors"].append("Owned server cleanup: " + str(error))
            finally:
                self.server.close()
        try:
            for name, home, filename in (("client", self.home, "khz_client.log"), ("server", self.server_home, "khz_server.log")):
                source = home / "main" / filename if home else None
                if source and source.is_file():
                    shutil.copy2(source, self.output / (name + "-native.log"))
            if self.home_authorized and self.home.exists() and cleanup["created_client_exited"]:
                self.home.rename(self.output / "fixture-client-home")
            if self.saved_home.exists() and not self.home.exists():
                self.saved_home.rename(self.home); cleanup["previous_home_restored"] = True
        except OSError as error:
            self.report["errors"].append("Fixture home/log preservation: " + str(error))
        if cleanup.get("client_forced_stop") or cleanup.get("server_forced_stop"):
            self.report["errors"].append("Native cleanup required forced termination")
        self.report["passed"] = bool(self.report["passed"] and not self.report["errors"] and cleanup["created_client_exited"] and cleanup.get("exact_owned_server_exited", False))
        self.report["elapsed_with_cleanup_seconds"] = time.monotonic() - self.started
        (self.output / "result.json").write_text(json.dumps(self.report, indent=2), encoding="utf8")


def run_checkpoint(fixture, before):
    report = fixture.report
    report["checkpoint_before"] = fixture.call("khz_checkpoint status", "KHZ_CHECKPOINT")
    reply = report["roundtrip"] = fixture.call("khz_checkpoint roundtrip", "KHZ_CHECKPOINT", 20)
    require_no_errors(checkpoint_errors(reply, before["stats"]["client_owner"]))
    report["checkpoint_after_discard"] = fixture.call("khz_checkpoint status", "KHZ_CHECKPOINT")
    # The successful result snapshots the pinned memento before discard. A later
    # status is deliberately unpinned; do not mistake it for another roundtrip.
    report["after_roundtrip"] = fixture.wait_ready("post-restoration gameplay", before)
    tape = report["authoritative_tape"] = {
        "all_records_retained": False,
        "verified_record_ordinals": [1, TAPE_TICKS],
        "scope": "Native completeness and first/last actual records; drain status does not retain or verify the remaining serialized records"
    }
    if fixture.retain_all_records:
        reject_empty_tape_export(fixture, tape)
    tape["begin"] = fixture.rcon("khz_server_capture begin 1000", "KHZ_SERVER_CAPTURE")
    if not integer(tape["begin"].get("fixture"), 1) or tape["begin"].get("target_ticks") != TAPE_TICKS or tape["begin"].get("active") is not True:
        raise ValueError("Native authoritative tape begin was rejected")
    for _ in range(5):
        tape["status"] = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
        if tape["status"].get("finished") is True:
            break
    require_no_errors(tape_status_errors(tape["status"]))
    if any(tape["status"].get(name) != tape["begin"].get(name) for name in ("fixture", "session", "server_id")):
        raise ValueError("Authoritative tape identity changed after begin")
    for ordinal, name in ((1, "first"), (TAPE_TICKS, "last")):
        tape[name] = fixture.rcon(f"khz_server_capture read {ordinal}", "KHZ_SERVER_CAPTURE_TICK", 3)
        require_no_errors(tape_record_errors(tape[name], tape["status"], ordinal))
    require_no_errors(record_pair_errors(tape["first"], tape["last"]))
    if fixture.retain_all_records:
        reject_owned_temporary_collision(fixture, tape)
        retain_tape_export(fixture, tape)
    tape["drain"] = fixture.rcon("khz_server_capture drain", "KHZ_SERVER_CAPTURE")
    require_no_errors(tape_status_errors(tape["drain"], True))
    tape["end"] = fixture.rcon("khz_server_capture end", "KHZ_SERVER_CAPTURE")
    require_no_errors(tape_status_errors(tape["end"], True))
    fixture.wait_ready("post-capture gameplay", report["after_roundtrip"], require_bounded_debt=fixture.retain_all_records)
    report["private_client_roundtrip_pass"] = report["authoritative_input_tape_pass"] = True


def run_profile(fixture):
    profile = fixture.report["profile"] = {"requested_seconds": 12, "overlapping_phases": True,
                                           "scope": "Short native phase and inclusive script-slice attribution; no timing acceptance claim"}
    fixture.rcon("khz_resetstats", fire=True)
    profile["server_before"] = fixture.native_server()
    profile["client_before"] = fixture.sample()
    started = time.monotonic()
    while time.monotonic() - started < 12:
        if not fixture.server.alive() or fixture.client.poll() is not None:
            raise RuntimeError("Owned process exited during short script profiling")
        time.sleep(min(.2, 12 - (time.monotonic() - started)))
    profile["measured_seconds"] = time.monotonic() - started
    profile["client_after"] = fixture.sample()
    require_no_errors(ready_errors(profile["client_after"], "mp_killhouse", fixture.client.pid, fixture.server.pid))
    profile["continuity"] = continuity(profile["client_before"], profile["client_after"])
    profile["server_after"] = fixture.native_server()
    metrics = profile["server_after"].get("server_phases", {})
    if metrics.get("overlapping_phases") is not True or not integer(metrics.get("samples"), 1):
        raise ValueError("Actual native server phase measurements are missing")
    slices = metrics.get("script_slices", {})
    if slices.get("enabled") is not True or slices.get("inclusive_nested") is not True or not slices.get("slowest"):
        raise ValueError("Actual native script profiling did not publish slice measurements")
    if not all(slice.get("source_available") is True and slice.get("file") for slice in slices["slowest"]):
        raise ValueError("Developer native script-source attribution is missing")
    profile["native_phase_report"] = metrics
    profile["passed"] = True


def self_test():
    server = {"peers": [{"port": 28994, "commands": 10, "snapshots": 10}],
              "native_bots": {"active": 15, "humans": 1, "difficulty": 1}}
    assert not server_ready_errors(server, 28994)
    assert server_ready_errors({**server, "native_bots": {"active": 2, "humans": 1, "difficulty": 1}}, 28994)
    assert server_ready_errors(server, 28993)
    clean_stats = {"presentation_state": {"renderer_started": True, "screen_initialized": True, "engine_error": False}}
    clean_ui = {"host": {"owned": False}, "top_menu": "main_text"}
    clean_state = {"connection": 0}
    assert leave_frontend_complete(clean_stats, clean_state, clean_ui, True)
    assert not leave_frontend_complete(clean_stats, clean_state, clean_ui, False)
    assert not leave_frontend_complete(clean_stats, clean_state, {**clean_ui, "top_menu": ""}, True)
    assert not leave_frontend_complete(clean_stats, {"connection": False}, clean_ui, True)
    assert not leave_frontend_complete(clean_stats, {"connection": 9}, clean_ui, True)
    assert not leave_frontend_complete({"presentation_state": {"renderer_started": False, "screen_initialized": True, "engine_error": False}}, clean_state, clean_ui, True)
    assert not leave_frontend_complete(clean_stats, clean_state, {**clean_ui, "host": {"owned": True}}, True)
    # One diagnostic timeout during actual Leave must not abort its bounded
    # frontend readiness wait or skip the eventual exact-child/menu checks.
    fixture = ManagedHostFixture.__new__(ManagedHostFixture)
    fixture.report, fixture.deadline = {}, time.monotonic() + 1
    fixture.call = lambda command: {"result": "queued"}
    fixture.server = type("StoppedOwnedChild", (), {"alive": lambda self: False})()
    receipts = iter((TimeoutError("UI assets reloading"), {"stats": clean_stats, "state": clean_state, "ui": clean_ui}))
    def leave_sample():
        value = next(receipts)
        if isinstance(value, Exception):
            raise value
        return value
    fixture.sample = leave_sample
    fixture.leave()
    assert fixture.report["last_leave_query_error"] == "UI assets reloading"
    assert fixture.report["explicit_leave_stopped_exact_server"] is True
    owner = {"session": 2, "generation": 3}
    image = dict(schema=1, pending=False, assets_pinned=True, fixture=1, session=2, generation=3, logical_tick=4,
                 provider_mask=1023, captured_mask=1023, validated_mask=1023, restored_mask=1023, required_domains=1023,
                 required_canonical_domains=127, roundtrip_domain_mask=127, storage_bytes=9, captures=1, validations=1,
                 restorations=1, failures=0, failure=0, reason="", complete=True, private_restore_complete=True)
    reply = dict(result="restored", stage="complete", fixture=1, cleanup=True, full_world_replay=False, checkpoint=image)
    assert not checkpoint_errors(reply, owner)
    for name, value in (("restored_mask", 511), ("pending", True), ("assets_pinned", None), ("failure", 1), ("generation", 4)):
        assert checkpoint_errors({**reply, "checkpoint": {**image, name: value}}, owner)
    assert checkpoint_errors({**reply, "full_world_replay": True}, owner)
    status = dict(schema=1, fixture=1, session=2, server_id=3, target_ticks=1000, capacity_ticks=1000, capacity_slots=16,
                  storage_bytes=8000, recorded=1000, drained=0, queued=1000, invalid=0, rejected=0, first_tick=10, last_tick=1009,
                  active=False, tick_open=False, finished=True, complete=True, export_complete=False,
                  verified_restore_conditions=0, full_world_replay_verified=False, failure="none")
    assert not tape_status_errors(status)
    assert not tape_status_errors({**status, "drained": 1000, "queued": 0, "export_complete": True}, True)
    for name, value in (("invalid", 1), ("last_tick", 1010), ("recorded", 999), ("complete", None), ("full_world_replay_verified", True)):
        assert tape_status_errors({**status, name: value})
    slots = [dict(slot=index, generation=index+1, source="held" if index == 0 else "native_bot", input_sequence_before=10,
                  input_sequence_after=10, input_advanced=False, applied_sequence=10, received_sequence=10, view_tick=10, consumed_commands=0,
                  entity_use_count=1, last_connect_time=1, last_spawn_time=1, server_id=3, script_id=index,
                  command=dict(server_time=20, buttons=0, angles=[0, 0, 0], weapon=1, offhand=0, forward=0, right=0,
                               melee_yaw=0., melee_distance=0, selected_location=[0, 0])) for index in range(16)]
    record = dict(schema=1, found=True, fixture=1, session=2, ordinal=1, tick=10, server_time=20, server_id=3,
                  active_mask_before=65535, active_mask_after=65535, recorded_mask=65535, complete=True,
                  verified_restore_conditions=0, failure="none", slots=slots)
    assert not tape_record_errors(record, status, 1)
    for name, value in (("session", 3), ("found", False), ("recorded_mask", 32767), ("slots", slots[:15]), ("ordinal", 2), ("schema", True), ("verified_restore_conditions", False)):
        assert tape_record_errors({**record, name: value}, status, 1)
    bad_slots = json.loads(json.dumps(slots)); bad_slots[1]["generation"] = 0
    assert tape_record_errors({**record, "slots": bad_slots}, status, 1)
    bad_slots = json.loads(json.dumps(slots)); bad_slots[1]["slot"] = None
    assert tape_record_errors({**record, "slots": bad_slots}, status, 1)
    bad_slots = json.loads(json.dumps(slots)); bad_slots[1]["command"]["melee_yaw"] = float("nan")
    assert tape_record_errors({**record, "slots": bad_slots}, status, 1)
    def record_at(ordinal):
        value = json.loads(json.dumps(record))
        value.update(ordinal=ordinal, tick=9+ordinal, server_time=19+ordinal)
        for slot in value["slots"]:
            slot["command"]["server_time"] = value["server_time"]
            slot["view_tick"] = value["tick"]
            if slot["source"] == "native_bot":
                slot["input_sequence_before"] = slot["input_sequence_after"] = value["tick"]
        return value
    last = record_at(1000)
    assert not tape_record_errors(last, status, 1000) and not record_pair_errors(record, last)
    assert record_pair_errors(record, {**last, "server_time": 1018})
    lines = [json.dumps(record_at(ordinal), separators=(",", ":")).encode("ascii") for ordinal in range(1, TAPE_TICKS+1)]
    raw = b"\n".join(lines) + b"\n"
    metadata = dict(schema=1, committed=True, file="main/khz_server_capture.jsonl", format="jsonl", records=1000,
                    fixture=1, session=2, server_id=3, first_tick=10, last_tick=1009, bytes=len(raw),
                    sha256=hashlib.sha256(raw).hexdigest(), elapsed_us=1000, failure="none", full_world_replay_verified=False)
    assert not tape_export_errors(metadata, status)
    for name, value in (("committed", False), ("fixture", 3), ("records", 999), ("bytes", 0), ("sha256", "bad"),
                        ("file", "../outside.jsonl"), ("elapsed_us", None), ("full_world_replay_verified", True)):
        assert tape_export_errors({**metadata, name: value}, status)
    result = validate_tape_jsonl(raw, status, record, last)
    assert result["records_validated"] == 1000 and result["slot_records_validated"] == 16000 and result["all_records_retained"] is True
    bad = list(lines); bad[500], bad[501] = bad[501], bad[500]
    changed = record_at(501); changed["slots"][1]["generation"] += 1
    altered = list(lines); altered[500] = json.dumps(changed).encode("ascii")
    for payload in (raw[:-1], b"\n".join(lines[:-1])+b"\n", b"\n".join(bad)+b"\n", b"\n".join(altered)+b"\n"):
        try:
            validate_tape_jsonl(payload, status, record, last)
        except ValueError:
            pass
        else:
            raise AssertionError("Incomplete, reordered or admission-mutated authoritative tape was accepted")
    assert debt_bounds([{"debt": 0}, {"debt": 2}])["debt_pass"]
    for values in ([{"debt": 0}, {"debt": 9}], [{"debt": 0}, {}], [{"debt": False}, {"debt": 0}]):
        try:
            debt_bounds(values)
        except ValueError:
            pass
        else:
            raise AssertionError("Growing or missing native scheduler debt was accepted")
    print("Private checkpoint, authoritative tape identity/masks/endpoints and false-attestation fixtures passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-port", type=int, default=28994)
    parser.add_argument("--action", choices=("checkpoint", "profile"), default="checkpoint")
    parser.add_argument("--retain-all-records", action="store_true", help="Require the Build 66+ native JSONL exporter and validate/retain all 1,000x16 actual records")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(); return 0
    if os.name != "nt" or not 1024 <= args.client_port <= 65535:
        parser.error("Actual native fixture requires Windows and port 1024..65535")
    project = Path(__file__).resolve().parent.parent
    run = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f") + f"-{os.getpid()}"
    output = (args.output or project / "results" / ("native-checkpoint-" + args.action + "-" + run)).resolve()
    if output.exists() or not output.is_relative_to(project / "results") or output == project / "results":
        parser.error("Output must be a new dedicated project results directory")
    for binary in (project / "cod4x1khz.exe", project / "runtime/cod4x1khz_server.exe"):
        if not binary.is_file():
            parser.error("Build the native project client/server first")
    output.mkdir(parents=True)
    fixture = ManagedHostFixture(project, output, args.client_port, args.action == "profile", retain_all_records=args.retain_all_records)
    try:
        before = fixture.start()
        if args.action == "checkpoint":
            run_checkpoint(fixture, before)
        else:
            run_profile(fixture)
        fixture.leave()
        fixture.report["passed"] = True
    except Exception as error:
        fixture.report["errors"].append(str(error))
    finally:
        fixture.report["test_elapsed_seconds"] = time.monotonic() - fixture.started
        fixture.close()
    print(json.dumps({"passed": fixture.report["passed"], "action": args.action, "errors": fixture.report["errors"],
                      "elapsed_seconds": fixture.report["elapsed_with_cleanup_seconds"], "output": str(output / "result.json")}, indent=2))
    return 0 if fixture.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
