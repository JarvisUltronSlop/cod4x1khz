"""Short stock-menu/owned-match bot evidence; never a timing or complete AI gate.

The default is one Regular DM match. --matrix runs the four menu difficulties,
the six stock rulesets, and an empty-bot match. Each capture is 1,000 actual
authoritative ticks. This script is intentionally unlaunched during benchmarks.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import time

from native_checkpoint_capture_probe import (ManagedHostFixture, integer,
    require_no_errors, tape_status_errors, tape_export_errors)
from native_fov_probe import (K_LEFT, K_RIGHT, stock_inventory, one_control, stock_action,
    wait_archived, archive_evidence)
from native_lifecycle_probe import (OwnedServer, continuity, server_continuity,
    native_map_matches, windows_for_pid, counter_delta)


MODES = ("dm", "war", "sd", "sab", "dom", "koth")
MENUS = tuple("settings_quick_" + mode for mode in (*MODES, "ctf"))
DIFFICULTIES = ("Recruit", "Regular", "Hardened", "Veteran")
SKILLS = (1, 3, 5, 7)  # Source contract; not an observed VM skill.
SKILL_TIMINGS = ((.6, 1500, 1000), (.4, 750, 500), (.25, 500, 300), (.1, 100, 50))
BUTTONS = {"attack": 0, "sprint": 1, "melee": 2, "use": 3,
           "reload": 4, "use_reload": 5, "lean_left": 6, "lean_right": 7,
           "prone": 8, "crouch": 9, "jump": 10, "ads": 11,
           "breath": 13, "frag": 14, "smoke": 15}
GAPS = {
    "actual_script_difficulty": "No existing endpoint reads each bot's actual pers['bots']['skill'] base/aim/reaction values. UI/server difficulty ordinal alone is configuration evidence.",
    "teams": "Existing native_bots and consumption tape do not expose native session team or actual script-team membership.",
    "combat_outcomes": "Selected attack/reload/grenade/weapon commands do not attest projectile emission, damage, reload completion or selected weapon outcome.",
    "human_reclaim": "This fixture admits one actual human but does not inject a second human or prove full-slot bot yielding and reuse.",
    "objectives": "No existing endpoint records bot goal selection, objective progress or completion. A loaded ruleset is not objective completion.",
    "typed_mode_inventory": "Six stock rulesets are explicit fixture cases. The UI has a CTF settings screen too; it is not assumed to be an available loaded game type.",
}


def setting(reply, field, maximum):
    value = reply.get("ui", {}).get(field)
    if not integer(value, 0, maximum):
        raise ValueError("Actual stock bot setting missing or malformed: " + field)
    return value


def menu_transition_errors(before, after, field, cardinality):
    try:
        old, new = setting(before, field, cardinality - 1), setting(after, field, cardinality - 1)
    except ValueError as error:
        return [str(error)]
    if new != (old + 1) % cardinality:
        return [f"Stock {field} right key did not change actual value {old} to {(old + 1) % cardinality}"]
    return []


def select_setting(fixture, menu, item, field, cardinality, target):
    if not integer(target, 0, cardinality - 1):
        raise ValueError("Invalid requested native menu selection")
    initial = fixture.call("khz_ui status")
    current = setting(initial, field, cardinality - 1)
    actions = []
    delta = (target - current) % cardinality
    # Even a default value gets an actual reversible key outcome, rather than
    # accepting an unchanged value as evidence that dispatch worked.
    directions = [1] * delta if delta else [1, -1]
    for direction in directions:
        before = fixture.call("khz_ui status")
        action = stock_action(fixture, menu, item, K_RIGHT if direction == 1 else K_LEFT)
        after = fixture.call("khz_ui status")
        if direction == 1:
            require_no_errors(menu_transition_errors(before, after, field, cardinality))
        elif setting(after, field, cardinality - 1) != (setting(before, field, cardinality - 1) - 1) % cardinality:
            raise ValueError("Actual stock left key did not reverse the observed right key")
        actions.append({"before": before["ui"], "after": after["ui"], "native_dispatch": action})
    final = fixture.call("khz_ui status")
    if setting(final, field, cardinality - 1) != target:
        raise ValueError("Actual stock menu selection differs from the requested value")
    archived = wait_archived(fixture, item["dvar"], expected=str(target))
    return {"initial": initial["ui"], "final": final["ui"], "actions": actions,
            "archive": archive_evidence(archived, settings=(item["dvar"],)), "passed": True}


def prepare_menu(fixture, menu, count, difficulty, exercise_all=False):
    opened = fixture.call("khz_ui menu " + menu)
    if opened.get("result") != "opened" or opened.get("top_menu") != menu:
        raise ValueError("Actual stock game-mode settings screen did not open: " + menu)
    items = stock_inventory(fixture, menu)
    count_item = one_control(items, dvar="ui_khz_botCount", kind=12, require_action=False)
    difficulty_item = one_control(items, dvar="ui_khz_botDifficulty", kind=12, require_action=False)
    evidence = {"menu": menu, "open": opened, "inventory": items, "selections": []}
    fixture.report.setdefault("bot_menu_evidence", []).append(evidence)
    if exercise_all:
        for value in (0, 15):
            evidence["selections"].append(select_setting(fixture, menu, count_item, "bot_count", 16, value))
        for value in range(4):
            evidence["selections"].append(select_setting(fixture, menu, difficulty_item, "bot_difficulty", 4, value))
    evidence["selections"].append(select_setting(fixture, menu, count_item, "bot_count", 16, count))
    evidence["selections"].append(select_setting(fixture, menu, difficulty_item, "bot_difficulty", 4, difficulty))
    evidence["passed"] = True
    return evidence


def bot_server_errors(stats, client_port, count, difficulty):
    errors = []
    bots = stats.get("native_bots", {})
    if bots.get("active") != count or bots.get("humans") != 1 or bots.get("requested") != count or bots.get("difficulty") != difficulty:
        errors.append("Actual native server population/settings differ from the menu-selected match")
    peers = [peer for peer in stats.get("peers", []) if peer.get("port") == client_port]
    if len(peers) != 1 or not integer(peers[0].get("commands"), 1) or not integer(peers[0].get("snapshots"), 1):
        errors.append("Actual human commands/snapshots were not admitted")
    slots = bots.get("slots")
    if type(slots) is not list or len(slots) != count:
        return errors + ["Actual native bot slot set differs"]
    ids = [slot.get("slot") for slot in slots if type(slot) is dict]
    if len(ids) != count or any(not integer(slot, 0, 15) for slot in ids) or len(set(ids)) != count:
        errors.append("Actual native bot slot ids are invalid or duplicated")
    for slot in slots:
        if type(slot) is not dict or any(not integer(slot.get(name), 1) for name in ("generation", "applied_tick", "command_ticks")):
            errors.append("Native bot generation or selected-command counters are missing")
    if count and bots.get("script_bootstrap") is not True:
        errors.append("Actual Bot Warfare script bootstrap is absent")
    return errors


def match_errors(sample, case, client_pid, child_pid):
    stats, state, ui = (sample.get(name, {}) for name in ("stats", "state", "ui"))
    owner, screen, host = stats.get("client_owner", {}), stats.get("presentation_state", {}), ui.get("host", {})
    errors = []
    if stats.get("process_id") != client_pid:
        errors.append("Actual diagnostic endpoint is not the created client")
    if any(type(value) is not int or value != 9 for value in (state.get("connection"), screen.get("connection"), owner.get("native_connection"))):
        errors.append("Actual main/owner native connections are not CA_ACTIVE")
    if not native_map_matches(state.get("map"), case["map"]):
        errors.append("Actual native map differs")
    if owner.get("active") is not True or owner.get("private_domains") is not True or owner.get("phase") != "running" or owner.get("native_cgame_initialized") is not True:
        errors.append("Actual private gameplay owner is not initialized and running")
    if any(not integer(owner.get(name), 1) for name in ("session", "generation", "ticks", "snapshots", "pose_ticks")):
        errors.append("Native ownership lifetime or tick/snapshot/pose counters are missing")
    if any(not integer(stats.get(name), 1) for name in ("commands_sent", "snapshots_received", "prediction_ticks", "gpu_presents")):
        errors.append("Actual client input/network/prediction/GPU counters are missing")
    if screen.get("renderer_started") is not True or screen.get("screen_initialized") is not True or screen.get("window_minimized") is not False or screen.get("engine_error") is not False:
        errors.append("Actual native presentation is not healthy")
    if host.get("owned") is not True or host.get("authenticated") is not True or host.get("phase") != "ready" or host.get("managed_server") is not False or host.get("child_pid") != child_pid:
        errors.append("Actual authenticated owned child is not ready")
    if any(host.get(name) != case[name] for name in ("map", "gametype", "bot_count", "bot_difficulty")):
        errors.append("Actual owned host map/mode/bot settings differ")
    if host.get("failures") != 0 or owner.get("queue_failures") != 0:
        errors.append("Native host or owner reported a failure")
    return errors


class BotFixture(ManagedHostFixture):
    def __init__(self, project, output, port, case, budget, menu_matrix):
        super().__init__(project, output, port, budget=budget)
        self.case, self.menu_matrix = case, menu_matrix
        self.report.update(case=case, probe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            scope="Stock native menu settings, actual owned match admission/continuity, and actual post-plugin selected bot commands. No timing, complete AI/difficulty/team/objective/combat gate.",
            complete_bot_acceptance=False, diagnostic_gaps=dict(GAPS))

    def native_server(self, require_ready=True):
        stats = self.rcon("khz_json")
        if stats.get("process_id") != self.server.pid:
            raise RuntimeError("RCON endpoint differs from the exact owned server")
        if require_ready:
            require_no_errors(bot_server_errors(stats, self.port, self.case["bot_count"], self.case["bot_difficulty"]))
        if stats.get("plugins", {}).get("loaded") != 0:
            raise ValueError("Bot fixture cannot contain loaded plugins")
        return stats

    def wait_ready(self, label, baseline=None, timeout=30, require_bounded_debt=False):
        stage = {"name": label, "samples": [], "server_samples": [], "passed": False}
        self.report.setdefault("readiness", []).append(stage)
        until, first, first_server = min(self.deadline, time.monotonic() + timeout), None, None
        while time.monotonic() < until:
            if self.server and not self.server.alive():
                raise RuntimeError("Exact owned child exited during bot fixture")
            try:
                sample = self.sample(); stage["samples"].append(sample)
                host = sample["ui"].get("host", {})
                if self.server is None and host.get("owned") is True and host.get("authenticated") is True and integer(host.get("child_pid"), 1):
                    if not integer(host.get("port"), 1, 65535):
                        raise ValueError("Owned child has no bounded actual port")
                    self.server = OwnedServer(host["child_pid"], self.server_binary)
                    self.server_port = host["port"]
                    self.server_home = self.project / "runtime/benchmarks" / f"server-{self.server_port}"
                    self.report["owned_server"] = {"pid": self.server.pid, "path": self.server.path, "port": self.server_port}
                errors = match_errors(sample, self.case, self.client.pid, self.server.pid if self.server else None)
                stage["last_errors"] = errors
                if errors:
                    first = first_server = None
                elif first is None:
                    first_server, first = self.native_server(), sample
                    stage["server_samples"].append(first_server)
                elif sample["elapsed"] - first["elapsed"] >= .3:
                    latest = self.native_server(); stage["server_samples"].append(latest)
                    stage["continuity"] = continuity(first, sample)
                    stage["server_continuity"] = server_continuity(first_server, latest, self.port)
                    for name in ("command_ticks",):
                        delta = counter_delta(first_server["native_bots"], latest["native_bots"], name)
                        if self.case["bot_count"] and delta <= 0:
                            raise ValueError("Native bot selected-command processing stopped")
                        stage["bot_" + name + "_delta"] = delta
                    stage["windows"] = windows_for_pid(self.client.pid)
                    if not any(window["visible"] and not window["minimized"] for window in stage["windows"]):
                        raise ValueError("Created bot fixture has no visible native window")
                    stage.update(passed=True, recovered=sample)
                    return sample
            except (ValueError, TimeoutError, OSError) as error:
                stage["last_error"] = str(error); first = first_server = None
            time.sleep(.1)
        raise TimeoutError(f"Bot readiness failed: {stage.get('last_errors')}; {stage.get('last_error')}")

    def start(self):
        # Preserve the same root EXE/foreign cwd/isolated profile contract as the
        # shared fixture, but drive bot menus before creating this match.
        if self.home.is_symlink() or self.home.is_junction():
            raise ValueError("Benchmark home is a reparse point")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", self.port))
        if self.home.exists():
            self.home.rename(self.saved_home); self.report["previous_home_preserved"] = True
        self.home_authorized = True
        arguments = [str(self.binary), "allowdupe", "+set", "net_port", str(self.port),
                     "+set", "khz_benchmark", "1", "+set", "khz_test_controls", "1",
                     "+set", "com_introPlayed", "1", "+set", "r_mode", "640x480",
                     "+set", "r_customMode", "640x480", "+set", "r_fullscreen", "0"]
        cfg = next((path for path in (self.project / "runtime/main/client.cfg", self.project / "runtime/client.cfg") if path.is_file()), None)
        if cfg:
            arguments += ["+exec", "client.cfg"]
            self.report["benchmark_cfg"] = {"path": str(cfg), "sha256": hashlib.sha256(cfg.read_bytes()).hexdigest()}
        arguments += ["+set", "com_maxfps", "0", "+set", "r_vsync", "0", "+setqport", str(self.port), "+set", "developer", "0"]
        self.report["launch"] = {"arguments": arguments, "cwd": r"C:\Windows\Temp", "auto_connect": False}
        self.client = subprocess.Popen(subprocess.list2cmdline(arguments), cwd=r"C:\Windows\Temp")
        self.report["owned_client_pid"] = self.client.pid
        until = min(self.deadline, time.monotonic() + 30)
        self.report["startup_samples"] = []
        while time.monotonic() < until:
            try:
                current = self.sample(); self.report["startup_samples"].append(current)
                if current["state"].get("connection") == 0 and current["ui"].get("top_menu") in ("main", "main_text"):
                    if current["ui"].get("host", {}).get("owned") is not False:
                        raise ValueError("Fresh disconnected startup already owns a server")
                    self.report["initial"] = current; break
            except (OSError, TimeoutError) as error:
                self.report["last_startup_error"] = str(error)
            time.sleep(.1)
        if "initial" not in self.report:
            raise TimeoutError("Fresh bot fixture did not reach native stock main menu")
        if self.menu_matrix:
            for menu in MENUS:
                prepare_menu(self, menu, self.case["bot_count"], self.case["bot_difficulty"], exercise_all=True)
        else:
            prepare_menu(self, "settings_quick_" + self.case["gametype"], self.case["bot_count"], self.case["bot_difficulty"])
        before = self.report["menu_settings_before_host"] = self.call("khz_ui status")
        # This existing adapter selects actual typed map/game-type entries and
        # invokes native StartServer. It sets identical already-proven settings;
        # the report does not pretend it is a menu key for selecting the mode.
        command = f"khz_ui host {self.case['map']} {self.case['gametype']} {self.case['bot_count']} {self.case['bot_difficulty']}"
        self.report["host_request"] = self.call(command)
        if self.report["host_request"].get("result") != "start requested":
            raise ValueError("Actual loaded typed map/mode StartServer request was rejected")
        if before["ui"]["bot_count"] != self.case["bot_count"] or before["ui"]["bot_difficulty"] != self.case["bot_difficulty"]:
            raise ValueError("Actual host request was not preceded by the tested stock bot selections")
        return self.wait_ready("menu-selected owned match")


def record_errors(record, status, ordinal, count):
    errors = []
    if type(record) is not dict or not integer(record.get("schema")) or record["schema"] != 1 or record.get("found") is not True or record.get("complete") is not True or record.get("failure") != "none" or not integer(record.get("verified_restore_conditions")) or record["verified_restore_conditions"] != 0:
        return ["Actual authoritative record is missing, incomplete, failed or falsely attests restoration"]
    for name in ("fixture", "session", "server_id"):
        if not integer(record.get(name), 1) or record[name] != status.get(name):
            errors.append("Actual record identity differs: " + name)
    if not integer(record.get("ordinal"), 1) or record["ordinal"] != ordinal or not integer(record.get("tick"), 1) or record["tick"] != status.get("first_tick", 0) + ordinal - 1 or not integer(record.get("server_time"), -2147483648, 2147483647):
        errors.append("Actual authoritative ordinal/tick/time is invalid")
    slots = record.get("slots")
    if type(slots) is not list or len(slots) != count + 1 or any(type(slot) is not dict or not integer(slot.get("slot"), 0, 15) for slot in slots):
        return errors + ["Actual tape does not retain the requested bounded native slot set"]
    ids = [slot["slot"] for slot in slots]
    mask = sum(1 << slot for slot in ids)
    if len(set(ids)) != len(ids) or any(not integer(record.get(name)) or record[name] != mask for name in ("active_mask_before", "active_mask_after", "recorded_mask")):
        errors.append("Actual active/recorded slot masks differ or contain duplicates")
    if sum(slot.get("source") == "native_bot" for slot in slots) != count or sum(slot.get("source") in ("queued", "held") for slot in slots) != 1:
        errors.append("Actual tape does not identify the requested bots and one actual human")
    for slot in slots:
        label = f"native slot {slot['slot']}"
        if not integer(slot.get("generation"), 1) or not integer(slot.get("script_id"), 0, 65535):
            errors.append(label + " lacks native admission/script identity")
        for name in ("entity_use_count", "last_connect_time", "last_spawn_time", "server_id"):
            if not integer(slot.get(name), -2147483648, 2147483647):
                errors.append(label + " has invalid native " + name)
        for name in ("input_sequence_before", "input_sequence_after", "applied_sequence", "received_sequence", "view_tick", "consumed_commands"):
            if not integer(slot.get(name)):
                errors.append(label + " has invalid selected-input " + name)
        if type(slot.get("input_advanced")) is not bool:
            errors.append(label + " lacks an actual advancement flag")
        command = slot.get("command")
        if type(command) is not dict:
            errors.append(label + " lacks an actual selected command"); continue
        if command.get("server_time") != record["server_time"] or not integer(command.get("buttons"), 0, (1 << 21) - 1):
            errors.append(label + " selected command time/buttons are invalid")
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
                errors.append(label + " selected-input metadata is inconsistent")
            if slot["source"] == "native_bot":
                if before != record["tick"] or after != record["tick"] or slot["consumed_commands"] != 0:
                    errors.append(label + " bot record does not identify this actual tick")
            elif after > slot["received_sequence"] or slot["applied_sequence"] > slot["received_sequence"] or (slot["source"] == "held") != (slot["consumed_commands"] == 0) or (slot["source"] == "held" and after != before):
                errors.append(label + " actual human admission/selection metadata is inconsistent")
    return errors


def analyze_records(records, status, count):
    if len(records) != 1000:
        raise ValueError("The actual command tape must retain all 1,000 ticks")
    observations, previous = {}, None
    for ordinal, record in enumerate(records, 1):
        require_no_errors(record_errors(record, status, ordinal, count))
        if previous and record["server_time"] != previous["server_time"] + 1:
            raise ValueError("Actual authoritative milliseconds are not consecutive")
        slots = {slot["slot"]: slot for slot in record["slots"]}
        if previous:
            prior = {slot["slot"]: slot for slot in previous["slots"]}
            if set(prior) != set(slots):
                raise ValueError("Actual slot set changed during retained tape")
            for slot_id, slot in slots.items():
                old = prior[slot_id]
                if slot["generation"] != old["generation"] or (slot["source"] == "native_bot") != (old["source"] == "native_bot") or slot["input_sequence_after"] < old["input_sequence_after"]:
                    raise ValueError("Actual admission identity or input sequence changed during tape")
        for slot in record["slots"]:
            if slot["source"] != "native_bot":
                continue
            command = slot["command"]
            key = str(slot["slot"])
            entry = observations.setdefault(key, {"generation": slot["generation"], "records": 0, "moving_ticks": 0,
                "buttons": {name: {"selected_ticks": 0, "rising_edges": 0, "max_held_ms": 0} for name in BUTTONS},
                "weapons": [], "offhands": [], "spawn_times": [], "spawn_time_changes": 0,
                "max_same_move_ms": 0, "_held": {}, "_move_run": 0, "_prior_move": None})
            entry["records"] += 1
            entry["moving_ticks"] += bool(command["forward"] or command["right"])
            move = (command["forward"], command["right"])
            entry["_move_run"] = entry["_move_run"] + 1 if move == entry["_prior_move"] else 1
            entry["_prior_move"] = move
            if any(move):
                entry["max_same_move_ms"] = max(entry["max_same_move_ms"], entry["_move_run"])
            for name, bit in BUTTONS.items():
                held = bool(command["buttons"] & (1 << bit))
                run = entry["_held"].get(name, 0)
                entry["buttons"][name]["selected_ticks"] += held
                entry["buttons"][name]["rising_edges"] += held and run == 0
                run = run + 1 if held else 0
                entry["_held"][name] = run
                entry["buttons"][name]["max_held_ms"] = max(entry["buttons"][name]["max_held_ms"], run)
            for name, field in (("weapons", "weapon"), ("offhands", "offhand")):
                if command[field] not in entry[name]:
                    entry[name].append(command[field])
            if not entry["spawn_times"] or entry["spawn_times"][-1] != slot["last_spawn_time"]:
                entry["spawn_time_changes"] += bool(entry["spawn_times"])
                entry["spawn_times"].append(slot["last_spawn_time"])
        previous = record
    for entry in observations.values():
        for field in ("_held", "_move_run", "_prior_move"):
            del entry[field]
    return {"records_validated": 1000, "slot_records_validated": 1000 * (count + 1),
            "actual_human_admission_observed": True, "stable_native_admissions": True,
            "bot_command_observations": observations, "scope": "Actual selected post-plugin native commands and observed native spawn-time changes; held windows are bounded by this tape, no combat outcome inference.",
            "complete_gameplay_coverage": False, "full_world_replay_verified": False}


def capture(fixture, number):
    tape = {"number": number}
    fixture.report.setdefault("command_tapes", []).append(tape)
    previous = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
    tape["begin"] = fixture.rcon("khz_server_capture begin 1000", "KHZ_SERVER_CAPTURE")
    begin = tape["begin"]
    if begin.get("active") is not True or begin.get("target_ticks") != 1000 or not integer(begin.get("fixture"), 1) or begin["fixture"] == previous.get("fixture") or begin.get("failure") != "none":
        raise ValueError("Actual authoritative tape begin did not create a new active capture")
    until = min(fixture.deadline, time.monotonic() + 8)
    while time.monotonic() < until:
        status = fixture.rcon("khz_server_capture status", "KHZ_SERVER_CAPTURE")
        tape.setdefault("status_samples", []).append(status)
        if status.get("finished") is True:
            tape["status"] = status; break
    if "status" not in tape:
        raise TimeoutError("Actual 1,000-tick command capture did not complete")
    require_no_errors(tape_status_errors(tape["status"]))
    if any(tape["status"].get(name) != begin.get(name) for name in ("fixture", "session", "server_id")):
        raise ValueError("Actual authoritative tape identity changed after begin")
    metadata = tape["export"] = fixture.rcon("khz_server_capture export", "KHZ_SERVER_CAPTURE_EXPORT", 10)
    require_no_errors(tape_export_errors(metadata, tape["status"]))
    source = fixture.server_home / "main/khz_server_capture.jsonl"
    if any(path.is_symlink() or path.is_junction() for path in (fixture.server_home, source.parent, source)) or not source.is_file() or source.resolve().parent != (fixture.server_home / "main").resolve():
        raise ValueError("Command tape export is not a confined owned regular file")
    with source.open("rb") as file:
        data = file.read(16384 * 1000 + 1)
    if len(data) != metadata["bytes"] or hashlib.sha256(data).hexdigest() != metadata["sha256"] or not data.endswith(b"\n"):
        raise ValueError("Actual exported command tape length/hash/termination differs")
    lines = data.splitlines()
    if any(not line or len(line) >= 16384 for line in lines):
        raise ValueError("Actual command tape has an empty or oversized record")
    records = [json.loads(line) for line in lines]
    tape["observations"] = analyze_records(records, tape["status"], fixture.case["bot_count"])
    inspected = fixture.report.get("actual_vm_team_inspection", {})
    if inspected.get("passed"):
        actual = {slot["slot"]: (slot["generation"], slot["source"] == "native_bot") for slot in records[0]["slots"]}
        expected = {slot["slot"]: (slot["generation"], slot["bot"]) for slot in inspected["samples"][-1]["slots"]}
        if actual != expected:
            raise ValueError("Actual native skill inspection and consumed-input tape admissions differ")
    destination = fixture.output / f"bot-commands-{number}.jsonl"
    destination.write_bytes(data)
    tape["retained_file"] = {"path": str(destination), "bytes": len(data), "sha256": metadata["sha256"]}
    tape["passed"] = True


def inspection_errors(value, case, session):
    errors = []
    if type(value) is not dict or not integer(value.get("schema")) or value["schema"] != 1 or value.get("complete") is not True or value.get("reason") != "":
        return ["Actual native bot inspection is absent, incomplete or rejected"]
    if not integer(value.get("session"), 1) or value["session"] != session or not integer(value.get("tick"), 1) or not integer(value.get("capacity")) or value["capacity"] != 16 or not integer(value.get("bot_count"), 0, 15) or value["bot_count"] != case["bot_count"] or not integer(value.get("human_count"), 1) or value["human_count"] != 1:
        errors.append("Actual native inspection lifetime/population differs")
    slots = value.get("slots")
    if type(slots) is not list or len(slots) != case["bot_count"] + 1 or any(type(slot) is not dict or not integer(slot.get("slot"), 0, 15) for slot in slots):
        return errors + ["Native inspection slot set is missing, duplicated or invalid"]
    if len({slot["slot"] for slot in slots}) != len(slots) or sum(slot.get("bot") is True for slot in slots) != case["bot_count"] or sum(slot.get("bot") is False for slot in slots) != 1:
        errors.append("Native inspection actual bot/human identities differ")
    teams = {0: 0, 1: 0, 2: 0, 3: 0}
    for slot in slots:
        if any(not integer(slot.get(name), 1) for name in ("generation", "entity_object", "pers_object")):
            errors.append("Native inspection has no actual entity/pers/admission identity")
        if not integer(slot.get("team"), 0, 3) or not integer(slot.get("session_state"), 0, 3):
            errors.append("Native inspection team/session state is invalid")
        else:
            teams[slot["team"]] += 1
            if slot["session_state"] not in (0, 1):
                errors.append("Native admitted player is not playing or dead in the ruleset")
            if slot["team"] not in ((0,) if case["gametype"] == "dm" else (1, 2)):
                errors.append("Native team membership does not match free-for-all/team rules")
        if not integer(slot.get("health"), -2147483648, 2147483647) or not integer(slot.get("weapon"), 0, 255):
            errors.append("Native inspection actual health/weapon is invalid")
        if slot.get("bot") is True:
            if any(not integer(slot.get(name), 1) for name in ("bots_object", "skill_object", "command_ticks", "applied_tick")):
                errors.append("Native bot skill object/control identity is missing")
            skill = slot.get("skill", {})
            expected_aim, expected_init, expected_reaction = SKILL_TIMINGS[case["bot_difficulty"]]
            if not integer(skill.get("base"), 1, 7) or skill["base"] != SKILLS[case["bot_difficulty"]]:
                errors.append("Actual native script-selected difficulty differs from the stock selection")
            if type(skill.get("aim_time")) not in (int, float) or not math.isfinite(skill["aim_time"]) or abs(skill["aim_time"] - expected_aim) > 1e-6:
                errors.append("Actual native bot aim timing differs from the selected skill")
            if any(not integer(skill.get(name)) or skill[name] != expected for name, expected in (("init_react_time", expected_init), ("reaction_time", expected_reaction))):
                errors.append("Actual native bot reaction settings differ from selected skill")
    if case["gametype"] != "dm" and case["bot_count"] and (not teams[1] or not teams[2]):
        errors.append("Actual team ruleset does not have native players on both teams")
    return errors


def inspect_actual_vm(fixture):
    # Opt-in only after this actual endpoint is linked. It is not inferred from
    # a dvar or silently substituted with the configuration-only bot stats.
    stage = {"passed": False, "samples": []}
    fixture.report["actual_vm_team_inspection"] = stage
    until = min(fixture.deadline, time.monotonic() + 8)
    baseline = fixture.report["readiness"][-1]["recovered"]["stats"]["client_owner"]["session"]
    while time.monotonic() < until:
        value = fixture.rcon("khz_bot_inspect", "KHZ_BOT_INSPECTION")
        stage["samples"].append(value)
        errors = inspection_errors(value, fixture.case, baseline)
        stage["last_errors"] = errors
        if not errors:
            stage.update(passed=True, native_team_counts={str(team): sum(slot["team"] == team for slot in value["slots"]) for team in range(4)},
                actual_bot_vm_skill_verified=True, native_ruleset_team_membership_verified=True,
                reaction_behavior_verified=False, objective_completion_verified=False)
            fixture.report["diagnostic_gaps"].pop("actual_script_difficulty", None)
            fixture.report["diagnostic_gaps"].pop("teams", None)
            fixture.report["diagnostic_gaps"]["reaction_behavior"] = "Actual native VM skill timing values verified; elapsed target acquisition/aim/fire behavior requires a controlled target fixture."
            return
    raise ValueError("Actual native skill/team inspection did not satisfy the selected ruleset: " + "; ".join(stage.get("last_errors", [])))


def cases(matrix):
    if not matrix:
        return [{"name": "regular_dm", "map": "mp_killhouse", "gametype": "dm", "bot_count": 15, "bot_difficulty": 1}]
    result = [{"name": name.lower() + "_dm", "map": "mp_killhouse", "gametype": "dm", "bot_count": 15, "bot_difficulty": index}
              for index, name in enumerate(DIFFICULTIES)]
    result += [{"name": "regular_" + mode, "map": "mp_killhouse", "gametype": mode, "bot_count": 15, "bot_difficulty": 1} for mode in MODES if mode != "dm"]
    result += [{"name": "empty_dm", "map": "mp_killhouse", "gametype": "dm", "bot_count": 0, "bot_difficulty": 1}]
    return result


def self_test():
    # These fixtures exercise the evidence validator, never pretend to run AI.
    a = {"ui": {"bot_count": 15, "bot_difficulty": 3}}
    b = {"ui": {"bot_count": 0, "bot_difficulty": 0}}
    assert not menu_transition_errors(a, b, "bot_count", 16)
    assert not menu_transition_errors(a, b, "bot_difficulty", 4)
    assert menu_transition_errors(a, a, "bot_count", 16)
    assert menu_transition_errors(a, {"ui": {"bot_count": True}}, "bot_count", 16)
    status = {"fixture": 1, "session": 2, "server_id": 3, "first_tick": 10}
    template = {"schema": 1, "found": True, "complete": True, "failure": "none", "verified_restore_conditions": 0,
                **status, "ordinal": 1, "tick": 10, "server_time": 100,
                "active_mask_before": 3, "active_mask_after": 3, "recorded_mask": 3}
    command = {"server_time": 100, "buttons": 1, "angles": [0, 0, 0], "weapon": 7, "offhand": 0,
               "forward": 127, "right": 0, "melee_yaw": 0., "melee_distance": 0, "selected_location": [0, 0]}
    bot = {"slot": 1, "generation": 4, "source": "native_bot", "script_id": 5, "entity_use_count": 1,
           "last_connect_time": 20, "last_spawn_time": 30, "server_id": 3, "input_sequence_before": 10,
           "input_sequence_after": 10, "input_advanced": False, "applied_sequence": 0,
           "received_sequence": 0, "view_tick": 0, "consumed_commands": 0, "command": command}
    human = {**bot, "slot": 0, "source": "held", "input_sequence_before": 3, "input_sequence_after": 3,
             "received_sequence": 3, "applied_sequence": 3}
    records = []
    for i in range(1000):
        records.append({**template, "ordinal": i + 1, "tick": 10 + i, "server_time": 100 + i,
            "slots": [{**human, "command": {**command, "server_time": 100 + i}},
                      {**bot, "input_sequence_before": 10 + i, "input_sequence_after": 10 + i,
                       "last_spawn_time": 30 if i < 500 else 60,
                       "command": {**command, "server_time": 100 + i, "buttons": 1 if i < 750 else 0}}]})
    proof = analyze_records(records, status, 1)["bot_command_observations"]["1"]
    assert proof["buttons"]["attack"] == {"selected_ticks": 750, "rising_edges": 1, "max_held_ms": 750}
    assert proof["spawn_time_changes"] == 1 and proof["moving_ticks"] == 1000 and proof["max_same_move_ms"] == 1000
    assert proof["weapons"] == [7]
    for field, value in (("complete", False), ("recorded_mask", 7), ("verified_restore_conditions", 1), ("tick", 9)):
        assert record_errors({**records[0], field: value}, status, 1, 1)
    for field, value in (("generation", 0), ("slot", True), ("input_advanced", True), ("source", "held")):
        assert record_errors({**records[0], "slots": [human, {**bot, field: value}]}, status, 1, 1)
    changed = json.loads(json.dumps(records)); changed[499]["slots"][1]["generation"] = 8
    try:
        analyze_records(changed, status, 1)
    except ValueError:
        pass
    else:
        raise AssertionError("A changed native bot admission passed")
    assert len(cases(True)) == 10 and set(row["gametype"] for row in cases(True)) == set(MODES)
    case = {"gametype": "dm", "bot_count": 1, "bot_difficulty": 1}
    inspected_bot = {"slot": 1, "bot": True, "generation": 4, "entity_object": 5, "pers_object": 6,
        "team": 0, "session_state": 0, "health": 100, "weapon": 7, "bots_object": 8, "skill_object": 9,
        "command_ticks": 1000, "applied_tick": 1000,
        "skill": {"base": 3, "aim_time": .4, "init_react_time": 750, "reaction_time": 500}}
    inspected_human = {**inspected_bot, "slot": 0, "bot": False}
    inspection = {"schema": 1, "complete": True, "reason": "", "session": 2, "tick": 1000,
        "capacity": 16, "bot_count": 1, "human_count": 1, "slots": [inspected_human, inspected_bot]}
    assert not inspection_errors(inspection, case, 2)
    for skill in ({**inspected_bot["skill"], "base": 1}, {**inspected_bot["skill"], "aim_time": .6},
                  {**inspected_bot["skill"], "reaction_time": 1000}):
        assert inspection_errors({**inspection, "slots": [inspected_human, {**inspected_bot, "skill": skill}]}, case, 2)
    assert inspection_errors(inspection, {**case, "gametype": "war"}, 2)
    assert inspection_errors({**inspection, "session": 3}, case, 2)
    print("Bot evidence validators passed: menu semantic changes, exact slot identity, actual held runs/spawn observations, corruption rejection")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-port", type=int, default=28992)
    parser.add_argument("--matrix", action="store_true", help="Ten short isolated cases; not a timing or complete AI gate")
    parser.add_argument("--case", choices=[case["name"] for case in cases(True)])
    parser.add_argument("--captures", type=int, default=1, help="One to three separate 1,000-tick actual tapes per case")
    parser.add_argument("--case-timeout", type=float, default=90, help="Overall per-case budget, excluding exact owned cleanup")
    parser.add_argument("--no-menu-matrix", action="store_true", help="Check only the selected mode's controls")
    parser.add_argument("--require-inspection", action="store_true", help="Require actual KHZ_BOT_INSPECTION VM/teams endpoint after explicit native integration")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(); return 0
    if os.name != "nt" or not 1024 <= args.client_port <= 65535 or not 1 <= args.captures <= 3 or not 30 <= args.case_timeout <= 180:
        parser.error("Windows, client port1024..65535, captures1..3, and per-case budget30..180 are required")
    project = Path(__file__).resolve().parent.parent
    output = (args.output or project / "results" / ("native-bots-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))).resolve()
    if output.parent != (project / "results").resolve() or output.exists() or output.is_symlink() or output.is_junction():
        parser.error("Output must be a new direct child of project/results")
    output.mkdir()
    selected = [case for case in cases(True) if case["name"] == args.case] if args.case else cases(args.matrix)
    report = {"schema": 1, "passed": False, "complete_bot_acceptance": False, "diagnostic_gaps": GAPS,
              "expected_skill_mapping_from_source": dict(zip(DIFFICULTIES, SKILLS)),
              "scope": "Scoped actual native menu/admission/selected-command evidence only. No complete bot, mode-objective, difficulty-response, human-reclaim or performance gate.",
              "cases": [], "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    for index, case in enumerate(selected):
        case_output = output / case["name"]; case_output.mkdir()
        fixture = BotFixture(project, case_output, args.client_port, case, args.case_timeout,
                             menu_matrix=index == 0 and not args.no_menu_matrix)
        fixture.report["native_inspection_required"] = args.require_inspection
        try:
            fixture.start()
            if args.require_inspection:
                inspect_actual_vm(fixture)
            for number in range(1, args.captures + 1):
                capture(fixture, number)
            fixture.wait_ready("post-capture actual match")
            fixture.leave()
            fixture.report["passed"] = True
        except Exception as error:
            fixture.report["errors"].append(str(error))
        finally:
            fixture.close()
        report["cases"].append({"name": case["name"], "passed": fixture.report["passed"],
                                 "result": str(case_output / "result.json"), "errors": fixture.report["errors"]})
        if not fixture.report["passed"]:
            # Do not start another fixture after failed cleanup or engine error.
            break
    report["passed"] = len(report["cases"]) == len(selected) and all(case["passed"] for case in report["cases"])
    (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    print(json.dumps({"passed": report["passed"], "complete_bot_acceptance": False,
                      "cases": len(report["cases"]), "output": str(output)}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
