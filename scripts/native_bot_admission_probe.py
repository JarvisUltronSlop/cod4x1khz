"""Short real second-human admission, full-slot bot reclaim and native refill proof.

Run only after the root agent authorizes an exclusive game fixture. This new
harness is recorded separately from the frozen build archive. Selected command
tapes attest native consumption, not combat outcomes or full-world replay.
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
from native_checkpoint_capture_probe import (ManagedHostFixture, integer,
    require_no_errors, tape_status_errors, tape_export_errors, leave_frontend_complete, debt_bounds)
from native_fov_probe import interactive_preferences
from native_lifecycle_probe import (OwnedServer, continuity, counter_delta,
    native_map_matches, ready_errors, server_continuity)
from native_bot_probe import BUTTONS


TICKS, SLOTS, ALL_SLOTS = 1000, 16, 65535


def life_state_errors(slot):
    """Admission is native CS_ACTIVE; stock death cameras have script state2."""
    if not integer(slot.get("team"), 0, 3) or slot["team"] != 0 or not integer(slot.get("session_state"), 0, 3):
        return ["Actual admission is not a bounded free-for-all life state"]
    if slot["session_state"] not in (0, 1, 2):
        return ["Actual free-for-all admission entered intermission"]
    if slot["session_state"] == 2 and (not integer(slot.get("health"), -2147483648, 0) or not integer(slot.get("last_spawn_time"), 1)):
        return ["Live or never-spawned spectator cannot substitute for an admitted stock death camera"]
    return []


def life_observations(samples):
    observed = {}
    for sample in samples:
        for slot in sample["inspection"].get("slots", []):
            if not integer(slot.get("slot"), 0, 15) or not integer(slot.get("generation"), 1) or type(slot.get("bot")) is not bool:
                continue
            key = f"{slot['slot']}:{'bot' if slot['bot'] else 'human'}:{slot['generation']}"
            value = observed.setdefault(key, {"states": [], "spawn_times": [], "observed_spawn_time_changes": 0})
            if slot.get("session_state") not in value["states"]:
                value["states"].append(slot.get("session_state"))
            spawn = slot.get("last_spawn_time")
            if not value["spawn_times"] or value["spawn_times"][-1] != spawn:
                value["observed_spawn_time_changes"] += bool(value["spawn_times"])
                value["spawn_times"].append(spawn)
    return {"native_admission_life_observations": observed, "full_respawn_behavior_verified": False}


def confined(path, root):
    """Check every existing ancestor before a fixture writes or moves a home."""
    if ".." in path.parts or ".." in root.parts:
        raise ValueError("Fixture path contains a lexical parent traversal")
    path, root = path.absolute(), root.absolute()
    if not path.is_relative_to(root):
        raise ValueError("Fixture path escapes the named project directory")
    cursor = path
    while True:
        if cursor.is_symlink() or cursor.is_junction():
            raise ValueError("Fixture path contains a reparse point: " + str(cursor))
        if cursor == root:
            break
        cursor = cursor.parent
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("Resolved fixture path escapes the named project directory")
    return path


def inspection_errors(value, bots, humans, session, server_id):
    errors = []
    for name, expected in (("schema", 1), ("capacity", SLOTS), ("bot_count", bots),
                           ("human_count", humans), ("session", session), ("server_id", server_id)):
        if not integer(value.get(name)) or value[name] != expected:
            errors.append("Actual idle-VM inspection differs: " + name)
    if value.get("complete") is not True or value.get("reason") != "" or value.get("full_bot_acceptance") is not False:
        errors.append("Actual idle-VM inspection is incomplete, rejected or overclaims acceptance")
    if not integer(value.get("tick"), 1) or not integer(value.get("server_time"), -2147483648, 2147483647):
        errors.append("Actual idle-VM inspection lacks native time")
    slots = value.get("slots")
    if type(slots) is not list or len(slots) != SLOTS or any(type(slot) is not dict or not integer(slot.get("slot"), 0, 15) for slot in slots):
        return errors + ["Actual idle-VM inspection does not contain all 16 bounded slots"]
    if sorted(slot["slot"] for slot in slots) != list(range(SLOTS)):
        errors.append("Actual idle-VM slots are duplicated or missing")
    if sum(slot.get("bot") is True for slot in slots) != bots or sum(slot.get("bot") is False for slot in slots) != humans:
        errors.append("Actual native bot/human kinds differ from settled admission")
    for slot in slots:
        label = f"native slot {slot['slot']}"
        if any(not integer(slot.get(name), 1) for name in ("generation", "entity_object", "pers_object")):
            errors.append(label + " lacks actual admission/entity/script identity")
        errors.extend(label + ": " + error for error in life_state_errors(slot))
        for name, low, high in (("health", -2147483648, 2147483647), ("weapon", 0, 255),
                                ("weapon_state", 0, 2147483647), ("last_connect_time", -2147483648, 2147483647),
                                ("last_spawn_time", -2147483648, 2147483647)):
            if not integer(slot.get(name), low, high):
                errors.append(label + " lacks native " + name)
        if slot.get("bot") is True:
            if any(not integer(slot.get(name), 1) for name in ("bots_object", "skill_object", "command_ticks", "applied_tick")):
                errors.append(label + " lacks actual bot skill/control identity")
            skill = slot.get("skill", {})
            aim = skill.get("aim_time")
            if skill.get("base") != 3 or not integer(skill.get("base")) or type(aim) not in (int, float) or not math.isfinite(aim) or abs(aim - .4) > 1e-6:
                errors.append(label + " does not have the native Regular base/aim setting")
            if skill.get("init_react_time") != 750 or skill.get("reaction_time") != 500 or any(not integer(skill.get(name)) for name in ("init_react_time", "reaction_time")):
                errors.append(label + " does not have native Regular reaction settings")
    return errors


def membership(value):
    return {slot["slot"]: (slot["bot"], slot["generation"]) for slot in value["slots"]}


def population_errors(stats, inspected, ports, bots, humans, child_pid):
    errors, native = [], stats.get("native_bots", {})
    if stats.get("process_id") != child_pid:
        errors.append("Diagnostic endpoint is not the exact retained child")
    if any(not integer(native.get(name)) for name in ("active", "humans", "requested", "difficulty")) or native.get("active") != bots or native.get("humans") != humans or native.get("requested") != 15 or native.get("difficulty") != 1 or native.get("script_bootstrap") is not True:
        errors.append("Actual native population/bootstrap or retained requested bot settings differ")
    slots = native.get("slots")
    expected = {slot["slot"]: slot["generation"] for slot in inspected.get("slots", []) if slot.get("bot") is True}
    if type(slots) is not list or any(type(slot) is not dict for slot in slots) or len(slots) != bots:
        errors.append("Actual native selected-control slot set is missing")
    else:
        actual = {slot.get("slot"): slot.get("generation") for slot in slots}
        if len(actual) != bots or actual != expected or any(not integer(slot.get(name), 1) for slot in slots for name in ("generation", "applied_tick", "command_ticks")):
            errors.append("Actual native controls and idle-VM admission identities differ")
    peers = stats.get("peers")
    if type(peers) is not list:
        return errors + ["Actual native server peers are absent"]
    admitted = []
    for port in ports:
        selected = [peer for peer in peers if type(peer) is dict and peer.get("port") == port]
        if len(selected) != 1 or not integer(selected[0].get("client"), 0, 15) or any(not integer(selected[0].get(name), 1) for name in ("commands", "snapshots")):
            errors.append("Actual accepted command/snapshot peer is missing for port " + str(port))
        else:
            admitted.append(selected[0]["client"])
    actual_humans = {slot["slot"] for slot in inspected["slots"] if slot["bot"] is False}
    if len(admitted) != humans or len(set(admitted)) != humans or set(admitted) != actual_humans:
        errors.append("Actual protocol peers do not match each native human admission")
    if stats.get("plugins", {}).get("loaded") != 0:
        errors.append("This native membership fixture must not contain loaded plugins")
    return errors


def reclaim_errors(before, joined, primary_slot, second_slot):
    a, b = membership(before), membership(joined)
    errors = []
    original_bots = [slot for slot, (bot, _) in a.items() if bot]
    if not original_bots or second_slot != max(original_bots) or a.get(second_slot, (False,))[0] is not True or b.get(second_slot, (True,))[0] is not False:
        errors.append("Second human did not reclaim the actual highest eligible native bot slot")
    if a.get(primary_slot) != b.get(primary_slot) or a.get(primary_slot, (True,))[0] is not False:
        errors.append("Existing primary human admission changed during reclaim")
    if {slot: identity for slot, identity in a.items() if slot != second_slot} != {slot: identity for slot, identity in b.items() if slot != second_slot}:
        errors.append("A surviving bot/human admission changed during reclaim")
    # Human and bot generations are separate namespaces. Their magnitudes are
    # deliberately not compared at this kind transition.
    return errors


def refill_errors(original, joined, refilled, second_slot):
    a, b, c = membership(original), membership(joined), membership(refilled)
    errors = []
    if c.get(second_slot, (False,))[0] is not True or c.get(second_slot, (False, 0))[1] <= a.get(second_slot, (False, 0))[1]:
        errors.append("Reclaimed slot did not receive a newer admission in the bot generation namespace")
    if {slot: identity for slot, identity in b.items() if slot != second_slot} != {slot: identity for slot, identity in c.items() if slot != second_slot}:
        errors.append("A surviving native admission changed during refill")
    return errors


def record_errors(record, status, ordinal, admitted):
    """Validate all actual slots without substituting the one-human tape schema."""
    errors = []
    if type(record) is not dict or record.get("schema") != 1 or not integer(record.get("schema")) or record.get("found") is not True or record.get("complete") is not True or record.get("failure") != "none" or record.get("verified_restore_conditions") != 0 or not integer(record.get("verified_restore_conditions")):
        return ["Actual native tape record is missing, incomplete, failed or overclaims restoration"]
    for name in ("fixture", "session", "server_id"):
        if not integer(record.get(name), 1) or record[name] != status.get(name):
            errors.append("Actual tape identity differs: " + name)
    if not integer(record.get("ordinal"), 1) or record["ordinal"] != ordinal or record.get("tick") != status.get("first_tick", 0) + ordinal - 1 or not integer(record.get("tick"), 1) or not integer(record.get("server_time"), -2147483648, 2147483647):
        errors.append("Actual authoritative ordinal/tick/time differs")
    if any(record.get(name) != ALL_SLOTS or not integer(record.get(name)) for name in ("active_mask_before", "active_mask_after", "recorded_mask")):
        errors.append("Actual authoritative tape does not contain every active native slot")
    slots = record.get("slots")
    if type(slots) is not list or len(slots) != SLOTS or any(type(slot) is not dict or not integer(slot.get("slot"), 0, 15) for slot in slots):
        return errors + ["Actual tape does not retain all 16 bounded native slot records"]
    if sorted(slot["slot"] for slot in slots) != list(range(SLOTS)):
        return errors + ["Actual tape slot set is missing or duplicated"]
    for slot in slots:
        label = f"native slot {slot['slot']}"
        source = slot.get("source")
        bot = source == "native_bot"
        if source not in ("native_bot", "queued", "held") or not integer(slot.get("generation"), 1) or admitted.get(slot["slot"]) != (bot, slot.get("generation")):
            errors.append(label + " source/admission differs from actual settled VM inspection")
        for name in ("input_sequence_before", "input_sequence_after", "applied_sequence", "received_sequence", "view_tick", "consumed_commands"):
            if not integer(slot.get(name)):
                errors.append(label + " lacks selected-input metadata " + name)
        if type(slot.get("input_advanced")) is not bool:
            errors.append(label + " lacks native advancement metadata")
        for name in ("entity_use_count", "last_connect_time", "last_spawn_time", "server_id"):
            if not integer(slot.get(name), -2147483648, 2147483647):
                errors.append(label + " lacks actual native " + name)
        if not integer(slot.get("script_id"), 0, 65535) or (not bot and slot.get("server_id") != status.get("server_id")):
            errors.append(label + " lacks native script identity or the human's accepted map identity")
        command = slot.get("command")
        if type(command) is not dict:
            errors.append(label + " lacks the actually selected command")
            continue
        if command.get("server_time") != record["server_time"] or not integer(command.get("server_time"), -2147483648, 2147483647) or not integer(command.get("buttons"), 0, (1 << 21) - 1):
            errors.append(label + " native command time/buttons are invalid")
        for name in ("weapon", "offhand", "melee_distance"):
            if not integer(command.get(name), 0, 255):
                errors.append(label + " command has invalid " + name)
        for name in ("forward", "right"):
            if not integer(command.get(name), -128, 127):
                errors.append(label + " command has invalid " + name)
        for name, length, low, high in (("angles", 3, -2147483648, 2147483647), ("selected_location", 2, -128, 127)):
            values = command.get(name)
            if type(values) is not list or len(values) != length or any(not integer(value, low, high) for value in values):
                errors.append(label + " command has invalid array " + name)
        yaw = command.get("melee_yaw")
        if type(yaw) not in (int, float) or not math.isfinite(yaw):
            errors.append(label + " command has invalid melee yaw")
        metadata = ("input_sequence_before", "input_sequence_after", "consumed_commands", "view_tick", "applied_sequence", "received_sequence")
        if all(integer(slot.get(name)) for name in metadata):
            before, after = slot["input_sequence_before"], slot["input_sequence_after"]
            if after < before or slot["input_advanced"] != (after > before) or slot["view_tick"] > record["tick"] or slot["consumed_commands"] > 2048:
                errors.append(label + " selected-input latch metadata is inconsistent")
            if bot:
                if before != record["tick"] or after != record["tick"] or slot["consumed_commands"] != 0 or slot["input_advanced"] is not False:
                    errors.append(label + " native bot consumed stale human input or does not identify the current tick")
            elif after > slot["received_sequence"] or slot["applied_sequence"] > slot["received_sequence"] or (source == "held") != (slot["consumed_commands"] == 0) or (source == "held" and after != before):
                errors.append(label + " human receipt/selection metadata is inconsistent")
    return errors


def analyze_records(records, status, inspected):
    if len(records) != TICKS:
        raise ValueError("Every one of the 1,000 actual authoritative records is required")
    admitted, prior, observations = membership(inspected), None, {}
    for ordinal, record in enumerate(records, 1):
        require_no_errors(record_errors(record, status, ordinal, admitted))
        if prior:
            if record["server_time"] != prior["server_time"] + 1:
                raise ValueError("Actual authoritative millisecond timeline is not consecutive")
            old = {slot["slot"]: slot for slot in prior["slots"]}
            if any(slot["input_sequence_after"] < old[slot["slot"]]["input_sequence_after"] for slot in record["slots"]):
                raise ValueError("Actual native input sequence decreased within stable admissions")
        for slot in record["slots"]:
            if slot["source"] != "native_bot":
                continue
            entry = observations.setdefault(str(slot["slot"]), {"generation": slot["generation"], "records": 0,
                "moving_ticks": 0, "buttons": {name: {"selected_ticks": 0, "rising_edges": 0, "max_held_ms": 0} for name in BUTTONS}, "_held": {}})
            entry["records"] += 1
            entry["moving_ticks"] += bool(slot["command"]["forward"] or slot["command"]["right"])
            for name, bit in BUTTONS.items():
                held, run = bool(slot["command"]["buttons"] & (1 << bit)), entry["_held"].get(name, 0)
                selected = entry["buttons"][name]
                selected["selected_ticks"] += held
                selected["rising_edges"] += held and run == 0
                run = run + 1 if held else 0
                entry["_held"][name] = run
                selected["max_held_ms"] = max(selected["max_held_ms"], run)
        prior = record
    for entry in observations.values():
        del entry["_held"]
    return {"records_validated": TICKS, "slot_records_validated": TICKS * SLOTS,
            "bot_command_observations": observations, "human_slots": sorted(slot for slot, (bot, _) in admitted.items() if not bot),
            "stable_actual_admissions": True, "full_world_replay_verified": False,
            "scope": "Actual selected post-plugin native commands; held windows are limited to this one-second tape, without combat outcome inference."}


class SecondClient:
    def __init__(self, fixture, port):
        self.fixture, self.port = fixture, port
        self.binary = fixture.project / "runtime/cod4x1khz_bench.exe"
        self.home = confined(fixture.project / "runtime/benchmarks" / f"client-{port}", fixture.project)
        self.saved_home = fixture.output / "previous-second-client-home"
        self.process = self.handle = None
        self.home_authorized = False
        self.report = fixture.report["second_client"] = {"port": port, "fresh_benchmark_home": str(self.home),
            "binary_path": str(self.binary), "binary_sha256": hashlib.sha256(self.binary.read_bytes()).hexdigest(),
            "headless_gpu_progress_required": False}

    def call(self, command, marker="KHZ_UI", timeout=1.2):
        if self.process is None or self.process.poll() is not None or (self.handle and not self.handle.alive()):
            raise RuntimeError("Exactly created second client is not alive")
        left = self.fixture.deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("Second-human fixture diagnostic budget exhausted")
        return query(self.port, command, marker, min(left, timeout))

    def sample(self):
        stats = self.call("khz_stats", "KHZ_JSON")
        if stats.get("process_id") != self.process.pid:
            raise RuntimeError("Second-client endpoint differs from its retained process identity")
        return {"elapsed": time.monotonic() - self.fixture.started, "stats": stats,
                "state": self.call("khz_state", "KHZ_STATE")}

    def start(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", self.port))
        if self.home.exists():
            self.home.rename(self.saved_home)
            self.report["previous_home_preserved"] = True
        self.home_authorized = True
        arguments = [str(self.binary), "allowdupe", "+set", "net_port", str(self.port),
            "+set", "khz_benchmark", "1", "+set", "khz_test_controls", "1", "+set", "com_introPlayed", "1",
            "+set", "r_mode", "640x480", "+set", "r_customMode", "640x480", "+set", "r_fullscreen", "0"]
        if any(path.is_file() for path in (self.fixture.project / "runtime/main/client.cfg", self.fixture.project / "runtime/client.cfg")):
            arguments += ["+exec", "client.cfg"]
        arguments += ["+set", "name", "khz-admission-second", "+set", "com_maxfps", "0", "+set", "r_vsync", "0",
            "+setqport", str(self.port), "+set", "developer", "0", "+set", "khz_profile_scripts", "0",
            "+connect", f"127.0.0.1:{self.fixture.server_port}"]
        self.report["launch"] = {"arguments": arguments, "cwd": r"C:\Windows\Temp"}
        self.process = subprocess.Popen(subprocess.list2cmdline(arguments), cwd=r"C:\Windows\Temp")
        self.report["created_pid"] = self.process.pid
        self.handle = OwnedServer(self.process.pid, self.binary)
        self.report["retained_process_handle_image"] = self.handle.path

    def leave(self):
        self.report["leave_request"] = self.call("khz_ui leave")
        if self.report["leave_request"].get("result") != "queued":
            raise ValueError("Second human's native disconnect was rejected")
        until = min(self.fixture.deadline, time.monotonic() + 10)
        samples = self.report["leave_samples"] = []
        while time.monotonic() < until:
            try:
                sample = self.sample()
                ui = self.call("khz_ui status")
                samples.append({**sample, "ui": ui})
                if leave_frontend_complete(sample["stats"], sample["state"], ui, True):
                    self.report["native_disconnected_frontend"] = True
                    return
            except (TimeoutError, OSError) as error:
                self.report["last_leave_query_error"] = str(error)
            time.sleep(.1)
        raise TimeoutError("Second human did not return to the actual disconnected frontend")

    def close(self):
        if self.report.get("cleanup_complete"):
            return
        cleanup = self.report.setdefault("cleanup", {})
        if self.process and self.process.poll() is None:
            try:
                if not self.report.get("native_disconnected_frontend"):
                    # Cleanup gets its own finite budget; a failed active test
                    # must still close only the process this fixture created.
                    stats = query(self.port, "khz_stats", "KHZ_JSON", 1)
                    if stats.get("process_id") != self.process.pid:
                        raise ValueError("Second cleanup endpoint is not the created client")
                    cleanup["leave"] = query(self.port, "khz_ui leave", "KHZ_UI", 1)
                    until = time.monotonic() + 10
                    while self.process.poll() is None and time.monotonic() < until:
                        try:
                            stats = query(self.port, "khz_stats", "KHZ_JSON", 1)
                            state = query(self.port, "khz_state", "KHZ_STATE", 1)
                            ui = query(self.port, "khz_ui status", "KHZ_UI", 1)
                            if stats.get("process_id") != self.process.pid:
                                raise ValueError("Second cleanup readiness process identity differs")
                            if leave_frontend_complete(stats, state, ui, True):
                                self.report["native_disconnected_frontend"] = True
                                break
                        except (TimeoutError, OSError) as error:
                            cleanup["last_leave_query_error"] = str(error)
                        time.sleep(.1)
                if self.process.poll() is None:
                    if not self.report.get("native_disconnected_frontend"):
                        raise TimeoutError("Second cleanup Leave did not finish frontend reload before Quit")
                    cleanup["quit"] = query(self.port, "khz_ui quit", "KHZ_UI", 1)
            except (TimeoutError, OSError, ValueError) as error:
                cleanup["query_error"] = str(error)
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                cleanup["forced_stop"] = True
                try:
                    if self.handle:
                        self.handle.stop()
                    else:
                        self.process.terminate()
                    self.process.wait(timeout=5)
                except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                    cleanup["stop_error"] = str(error)
                    self.fixture.report["errors"].append("Exact second-client termination: " + str(error))
        cleanup["exact_created_client_exited"] = self.process is None or self.process.poll() is not None
        cleanup["returncode"] = self.process.returncode if self.process else None
        if self.handle:
            cleanup["retained_handle_signalled"] = not self.handle.alive()
            if cleanup["retained_handle_signalled"]:
                self.handle.close()
                self.handle = None
        if cleanup.get("forced_stop") or (self.process and self.process.returncode):
            self.fixture.report["errors"].append("Second native client required forced termination or exited with an error")
        if self.home_authorized and cleanup["exact_created_client_exited"]:
            try:
                if self.home.exists():
                    confined(self.home, self.fixture.project)
                    logs = list((self.home / "main").glob("khz_bench_*.log"))
                    for source in logs:
                        confined(source, self.fixture.project)
                        if source.is_file():
                            shutil.copy2(source, self.fixture.output / ("second-" + source.name))
                    self.home.rename(self.fixture.output / "fixture-second-client-home")
                if self.saved_home.exists() and not self.home.exists():
                    self.saved_home.rename(self.home)
                    cleanup["previous_home_restored"] = True
            except (OSError, ValueError) as error:
                self.fixture.report["errors"].append("Second fixture home/log preservation: " + str(error))
        self.report["cleanup_complete"] = cleanup["exact_created_client_exited"]


def second_errors(sample, pid, server_id):
    stats, state = sample.get("stats", {}), sample.get("state", {})
    errors = []
    if stats.get("process_id") != pid or state.get("connection") != 9 or not native_map_matches(state.get("map"), "mp_killhouse") or state.get("server_id") != server_id:
        errors.append("Actual second protocol client identity/CA_ACTIVE/map/serverId differs")
    for name in ("commands_sent", "snapshots_received", "prediction_ticks", "ack_input"):
        if not integer(stats.get(name), 1):
            errors.append("Second protocol client lacks actual " + name)
    return errors


def second_continuity(first, last):
    result = {name: counter_delta(first["stats"], last["stats"], name)
              for name in ("commands_sent", "snapshots_received", "prediction_ticks", "ack_input")}
    if not all(value > 0 for value in result.values()):
        raise ValueError("Second client transport attempts, prediction or actual authoritative input acknowledgement stopped")
    return result


def wait_population(fixture, second, label, bots, humans, expected=None, timeout=12):
    stage = {"name": label, "passed": False, "samples": []}
    fixture.report.setdefault("membership_stages", []).append(stage)
    until, first = min(fixture.deadline, time.monotonic() + timeout), None
    while time.monotonic() < until:
        try:
            primary = fixture.sample()
            require_no_errors(ready_errors(primary, "mp_killhouse", fixture.client.pid, fixture.server.pid))
            owner = primary["stats"]["client_owner"]
            inspected = fixture.rcon("khz_bot_inspect", "KHZ_BOT_INSPECTION")
            server = fixture.native_server(require_ready=False)
            entry = {"primary": primary, "inspection": inspected, "server": server}
            stage["samples"].append(entry)
            require_no_errors(inspection_errors(inspected, bots, humans, owner["session"], primary["state"]["server_id"]))
            ports = [fixture.port] + ([second.port] if humans == 2 else [])
            require_no_errors(population_errors(server, inspected, ports, bots, humans, fixture.server.pid))
            if expected is not None and membership(inspected) != expected:
                raise ValueError("Actual settled admission identity changed")
            if humans == 2:
                entry["second"] = second.sample()
                require_no_errors(second_errors(entry["second"], second.process.pid, inspected["server_id"]))
            if first is None or membership(inspected) != membership(first["inspection"]):
                first = entry
                continue
            stage["primary_continuity"] = continuity(first["primary"], primary)
            stage["primary_server_continuity"] = server_continuity(first["server"], server, fixture.port)
            stage["primary_debt"] = debt_bounds([first["primary"]["stats"], primary["stats"]])
            stage["server_debt"] = debt_bounds([first["server"], server])
            before = {slot["slot"]: slot for slot in first["server"]["native_bots"]["slots"]}
            stage["per_bot_command_deltas"] = {str(slot["slot"]): counter_delta(before[slot["slot"]], slot, "command_ticks") for slot in server["native_bots"]["slots"]}
            if not all(value > 0 for value in stage["per_bot_command_deltas"].values()):
                raise ValueError("At least one admitted native bot stopped consuming selected controls")
            if humans == 2:
                stage["second_continuity"] = second_continuity(first["second"], entry["second"])
                stage["second_server_continuity"] = server_continuity(first["server"], server, second.port)
                stage["second_debt"] = debt_bounds([first["second"]["stats"], entry["second"]["stats"]])
            stage.update(passed=True, settled=entry, **life_observations(stage["samples"]))
            return entry
        except (TimeoutError, OSError, ValueError) as error:
            stage["last_error"] = str(error)
            first = None
        time.sleep(.1)
    raise TimeoutError(label + " did not reach actual admitted continuity: " + stage.get("last_error", "no valid sample"))


def capture_membership(fixture, second, label, settled, humans):
    tape = {"name": label, "membership": {str(slot): {"bot": bot, "generation": generation} for slot, (bot, generation) in membership(settled["inspection"]).items()}, "passed": False}
    fixture.report.setdefault("command_tapes", []).append(tape)
    previous = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
    begin = tape["begin"] = fixture.rcon("khz_server_capture begin 1000", "KHZ_SERVER_CAPTURE")
    if begin.get("active") is not True or begin.get("target_ticks") != TICKS or not integer(begin.get("fixture"), 1) or begin["fixture"] == previous.get("fixture") or begin.get("failure") != "none":
        raise ValueError("Actual authoritative begin did not create a new 1,000-tick capture")
    until = min(fixture.deadline, time.monotonic() + 8)
    while time.monotonic() < until:
        status = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
        tape.setdefault("status_samples", []).append(status)
        if status.get("finished") is True:
            tape["status"] = status
            break
    if "status" not in tape:
        raise TimeoutError("Actual selected-command tape did not finish")
    require_no_errors(tape_status_errors(tape["status"]))
    if any(tape["status"].get(name) != begin.get(name) for name in ("fixture", "session", "server_id")):
        raise ValueError("Actual tape lifetime changed during capture")
    if tape["status"].get("session") != settled["inspection"]["session"] or tape["status"].get("server_id") != settled["inspection"]["server_id"]:
        raise ValueError("Actual tape does not use the inspected native host lifetime")
    metadata = tape["export"] = fixture.rcon("khz_server_capture export", "KHZ_SERVER_CAPTURE_EXPORT", 10)
    require_no_errors(tape_export_errors(metadata, tape["status"]))
    source = confined(fixture.server_home / "main/khz_server_capture.jsonl", fixture.project)
    if not source.is_file():
        raise ValueError("Actual exported tape is not an owned benchmark regular file")
    with source.open("rb") as file:
        data = file.read(16384 * TICKS + 1)
    if len(data) != metadata["bytes"] or hashlib.sha256(data).hexdigest() != metadata["sha256"] or not data.endswith(b"\n"):
        raise ValueError("Actual retained tape bytes/hash/termination differ from the native commit")
    destination = fixture.output / (label + "-commands.jsonl")
    destination.write_bytes(data)  # Preserve failed-schema bytes as well.
    tape["retained_file"] = {"path": str(destination), "bytes": len(data), "sha256": metadata["sha256"]}
    lines = data.splitlines()
    if any(not line or len(line) >= 16384 for line in lines):
        raise ValueError("Actual tape contains an empty or oversized record")
    tape["observations"] = analyze_records([json.loads(line) for line in lines], tape["status"], settled["inspection"])
    # Native begin deliberately refuses an undrained retained ledger. Contents
    # were already read, retained and validated above; draining only retires
    # that consumed ledger so the next membership can record a new fixture.
    tape["drain"] = fixture.rcon("khz_server_capture drain", "KHZ_SERVER_CAPTURE")
    require_no_errors(tape_status_errors(tape["drain"], True))
    if any(tape["drain"].get(name) != tape["status"].get(name) for name in
           ("fixture", "session", "server_id", "recorded", "first_tick", "last_tick")):
        raise ValueError("Draining the validated tape changed its actual ledger identity")
    tape["drain_scope"] = "Retires the already retained and validated ledger; no serialized contents are inferred from drain status."
    tape["recovery"] = wait_population(fixture, second, "after " + label + " export", 16 - humans, humans,
                                        expected=membership(settled["inspection"]), timeout=10)
    tape["intentional_off_tick_export_stall_us"] = metadata["elapsed_us"]
    tape["passed"] = True


def provenance(project, archive_path):
    archive_path = confined(archive_path, project)
    data = archive_path.read_bytes()
    if len(data) > 2 * 1024 * 1024:
        raise ValueError("Source archive is oversized")
    archive = json.loads(data)
    if not integer(archive.get("build"), 1) or archive.get("tests", {}).get("passed") is not True:
        raise ValueError("Explicit archive has no successful native build/test identity")
    actual = {}
    for name in ("cod4x1khz.exe", "runtime/cod4x1khz_server.exe", "runtime/cod4x1khz_bench.exe"):
        digest = hashlib.sha256(confined(project / name, project).read_bytes()).hexdigest()
        if archive.get("binary_sha256", {}).get(name) != digest:
            raise ValueError("Actual fixture binary differs from the explicit frozen archive: " + name)
        actual[name] = digest
    assets = {}
    if "runtime/main/zz_cod4x1khz_bots.iwd" not in archive.get("runtime_asset_packages", {}):
        raise ValueError("Explicit archive does not identify the actual Bot Warfare package")
    for name, expected in archive.get("runtime_asset_packages", {}).items():
        digest = hashlib.sha256(confined(project / name, project).read_bytes()).hexdigest()
        if digest != expected:
            raise ValueError("Actual runtime package differs from the frozen archive: " + name)
        assets[name] = digest
    sources = {}
    for name in ("scripts/monitor.py", "scripts/native_checkpoint_capture_probe.py", "scripts/native_lifecycle_probe.py", "scripts/native_fov_probe.py", "scripts/native_bot_probe.py"):
        digest = hashlib.sha256(confined(project / name, project).read_bytes()).hexdigest()
        if archive.get("source_sha256", {}).get(name) != digest:
            raise ValueError("Imported frozen helper differs from source archive: " + name)
        sources[name] = digest
    harness_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    archived_harness = archive.get("source_sha256", {}).get("scripts/native_bot_admission_probe.py")
    return {"build": archive.get("build"), "archive_path": str(archive_path), "archive_sha256": hashlib.sha256(data).hexdigest(),
            "binary_sha256": actual, "asset_sha256": assets, "imported_helper_sha256": sources,
            "new_harness_path": str(Path(__file__).resolve()), "new_harness_sha256": harness_hash,
            "new_harness_in_frozen_archive": archived_harness is not None,
            "archived_harness_sha256": archived_harness,
            "new_harness_matches_frozen_archive": archived_harness == harness_hash if archived_harness is not None else None,
            "scope": "New harness provenance is separate; it never replaces the deployed build/source archive."}


def self_test():
    import copy
    valid_death = {"team": 0, "session_state": 2, "health": 0, "last_spawn_time": 123}
    assert not life_state_errors(valid_death)
    assert not life_state_errors({**valid_death, "health": -1})
    assert life_state_errors({**valid_death, "health": 100})
    assert life_state_errors({**valid_death, "last_spawn_time": 0})
    assert life_state_errors({**valid_death, "team": 3})
    assert life_state_errors({**valid_death, "session_state": 3})
    assert life_state_errors({**valid_death, "health": False})
    root = Path(__file__).resolve().parents[1]
    assert confined(root / "results" / "future-admission-artifact", root).is_relative_to(root)
    for unsafe in (root / "results" / ".." / ".." / "escaped", root.parent / "outside-admission-fixture"):
        try:
            confined(unsafe, root)
        except ValueError:
            pass
        else:
            raise AssertionError("Unconfined fixture path was accepted")
    slots = [{"slot": slot, "bot": slot != 0, "generation": 100 + slot} for slot in range(SLOTS)]
    original = {"slots": slots}
    joined = copy.deepcopy(original)
    joined["slots"][15].update(bot=False, generation=1)
    assert not reclaim_errors(original, joined, 0, 15)
    wrong = copy.deepcopy(joined)
    wrong["slots"][14]["generation"] += 1
    assert reclaim_errors(original, wrong, 0, 15)
    refilled = copy.deepcopy(joined)
    refilled["slots"][15].update(bot=True, generation=116)
    assert not refill_errors(original, joined, refilled, 15)
    wrong = copy.deepcopy(refilled)
    wrong["slots"][15]["generation"] = 115
    assert refill_errors(original, joined, wrong, 15)
    first = {"stats": {"commands_sent": 1, "snapshots_received": 1, "prediction_ticks": 1, "ack_input": 1}}
    last = {"stats": {"commands_sent": 10, "snapshots_received": 10, "prediction_ticks": 10, "ack_input": 2}}
    assert second_continuity(first, last)["ack_input"] == 1
    last["stats"]["ack_input"] = 1
    try:
        second_continuity(first, last)
    except ValueError:
        pass
    else:
        raise AssertionError("Frozen authoritative ack accepted as actual human admission")
    status = {"fixture": 1, "session": 2, "server_id": 3, "first_tick": 10}
    command = {"server_time": 100, "buttons": 1, "angles": [0, 0, 0], "weapon": 7, "offhand": 0,
               "forward": 127, "right": 0, "melee_yaw": 0., "melee_distance": 0, "selected_location": [0, 0]}
    record = {"schema": 1, "found": True, "complete": True, "failure": "none", "verified_restore_conditions": 0,
              **status, "ordinal": 1, "tick": 10, "server_time": 100,
              "active_mask_before": ALL_SLOTS, "active_mask_after": ALL_SLOTS, "recorded_mask": ALL_SLOTS, "slots": []}
    for slot, (bot, generation) in membership(joined).items():
        record["slots"].append({"slot": slot, "generation": generation, "source": "native_bot" if bot else "held", "script_id": 5,
            "entity_use_count": 1, "last_connect_time": 20, "last_spawn_time": 30, "server_id": 3,
            "input_sequence_before": 10 if bot else 3, "input_sequence_after": 10 if bot else 3,
            "input_advanced": False, "applied_sequence": 0 if bot else 3, "received_sequence": 0 if bot else 3,
            "view_tick": 0, "consumed_commands": 0, "command": dict(command)})
    assert not record_errors(record, status, 1, membership(joined))
    for name, value in (("generation", 999), ("source", "held"), ("input_advanced", True), ("consumed_commands", 1)):
        wrong = copy.deepcopy(record)
        wrong["slots"][1][name] = value
        assert record_errors(wrong, status, 1, membership(joined)), name
    wrong = copy.deepcopy(record)
    wrong["slots"].pop()
    assert record_errors(wrong, status, 1, membership(joined))
    print("Admission namespace, stable membership, complete 16-slot tape and accepted-ack schema policies passed; no actual game proof")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-archive", type=Path, help="Explicit deployed results/buildNN-source-and-binary-hashes.json")
    parser.add_argument("--client-port", type=int, default=28988)
    parser.add_argument("--second-port", type=int, default=28989)
    parser.add_argument("--seconds", type=int, default=85, help="Active diagnostic budget; exact owned cleanup follows on all paths")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if os.name != "nt":
        parser.error("Actual fixture requires Windows")
    if not args.build_archive:
        parser.error("--build-archive is required for an explicit frozen deployment")
    if any(not 1024 <= port <= 65535 for port in (args.client_port, args.second_port)) or args.client_port == args.second_port or not 45 <= args.seconds <= 90:
        parser.error("Use two distinct ports in 1024..65535 and a 45..90 second diagnostic budget")
    project = args.project.resolve()
    output = args.output or project / "results" / ("native-bot-admission-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))
    output = confined(output, project)
    output.mkdir(parents=True, exist_ok=False)
    archive = args.build_archive if args.build_archive.is_absolute() else project / args.build_archive
    fixture = second = None
    prefs = None
    try:
        before = provenance(project, archive)
        prefs = interactive_preferences(project)
        for port in (args.client_port, args.second_port):
            confined(project / "runtime/benchmarks" / f"client-{port}", project)
        fixture = ManagedHostFixture(project, output, args.client_port, budget=args.seconds)
        fixture.report.update(provenance_before_launch=before, interactive_preferences_before=prefs,
            scope="Actual second custom-protocol human admission at capacity, native bot slot reclaim/refill, and complete settled 16-slot selected-command tapes. No timing, combat outcomes, AI reaction/objective or full-world replay acceptance.",
            complete_bot_acceptance=False, profile_persistence_relaunch_verified=False,
            new_admission_harness_sha256=before["new_harness_sha256"])
        second = SecondClient(fixture, args.second_port)
        fixture.start()
        baseline = wait_population(fixture, second, "full original match", 15, 1)
        second.start()
        joined = wait_population(fixture, second, "second human admitted", 14, 2)
        def peer_slot(value, port):
            return next(peer["client"] for peer in value["server"]["peers"] if peer.get("port") == port)
        primary_slot, second_slot = peer_slot(joined, fixture.port), peer_slot(joined, second.port)
        require_no_errors(reclaim_errors(baseline["inspection"], joined["inspection"], primary_slot, second_slot))
        fixture.report["reclaim"] = {"passed": True, "primary_slot": primary_slot, "reclaimed_slot": second_slot,
            "original_bot_generation": membership(baseline["inspection"])[second_slot][1],
            "second_human_generation": membership(joined["inspection"])[second_slot][1],
            "generation_namespaces_compared_separately": True}
        capture_membership(fixture, second, "joined-two-human", joined, 2)
        second.leave()
        refilled = wait_population(fixture, second, "native bot refill", 15, 1)
        require_no_errors(refill_errors(baseline["inspection"], joined["inspection"], refilled["inspection"], second_slot))
        fixture.report["refill"] = {"passed": True, "slot": second_slot,
            "new_bot_generation": membership(refilled["inspection"])[second_slot][1],
            "same_owned_server_pid": fixture.server.pid}
        capture_membership(fixture, second, "refilled-one-human", refilled, 1)
        second.close()
        fixture.leave()
        fixture.report["passed"] = True
    except Exception as error:
        if fixture:
            fixture.report["errors"].append(str(error))
        else:
            (output / "result.json").write_text(json.dumps({"passed": False, "errors": [str(error)], "game_started": False}, indent=2), encoding="utf8")
    finally:
        if fixture:
            if second:
                try:
                    second.close()
                except Exception as error:
                    fixture.report["errors"].append("Exact second-client cleanup: " + str(error))
            fixture.close()
            try:
                fixture.report["provenance_after_cleanup"] = provenance(project, archive)
                fixture.report["binary_asset_helper_provenance_unchanged"] = fixture.report.get("provenance_before_launch") == fixture.report["provenance_after_cleanup"]
                fixture.report["interactive_preferences_after"] = interactive_preferences(project)
                fixture.report["interactive_preferences_unchanged"] = prefs == fixture.report["interactive_preferences_after"]
                if not fixture.report["binary_asset_helper_provenance_unchanged"] or not fixture.report["interactive_preferences_unchanged"]:
                    fixture.report["errors"].append("Frozen provenance or interactive preferences changed")
            except Exception as error:
                fixture.report["errors"].append("After-cleanup evidence: " + str(error))
            fixture.report["passed"] = bool(fixture.report["passed"] and not fixture.report["errors"] and
                second is not None and second.report.get("cleanup", {}).get("exact_created_client_exited") is True and
                not second.report.get("cleanup", {}).get("forced_stop"))
            (output / "result.json").write_text(json.dumps(fixture.report, indent=2), encoding="utf8")
    print(output / "result.json")
    return 0 if fixture and fixture.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
