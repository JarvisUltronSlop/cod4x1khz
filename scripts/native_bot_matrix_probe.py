"""Actual four-difficulty/six-mode bot evidence and isolated profile persistence.

Run only in an authorized exclusive native-game window. Existing archived
helpers are imported unchanged. Every retained tape is consumed before its
ledger is drained; no objective, combat, reaction or full-world replay claim is
derived from a mode name, a configured dvar, or a selected button command.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import time

from native_checkpoint_capture_probe import (ManagedHostFixture, integer,
    require_no_errors, tape_status_errors, leave_frontend_complete, debt_bounds)
from native_bot_probe import (MODES, DIFFICULTIES, SKILLS, SKILL_TIMINGS,
    prepare_menu, match_errors, bot_server_errors, capture)
from native_bot_admission_probe import confined, provenance as archived_provenance
from native_fov_probe import interactive_preferences, archived_config, archive_evidence
from native_lifecycle_probe import (OwnedServer, continuity, server_continuity,
    counter_delta, windows_for_pid)


PENDING = {
    "reaction_behavior": "Actual VM timing values and selected controls are observed; elapsed target acquisition/aim/fire response is not measured.",
    "objective_completion": "Native movement/aim goals and stock scores are retained; objective selection/progress/completion is not exposed by the current endpoint.",
    "combat_outcomes": "Health, native score lines, spawn-time changes and selected attack/reload/grenade commands are observations, not controlled emission/damage/reload-completion proof.",
    "script_team_mode": "The endpoint validates actual script pers/skill objects and native session teams; pers['team'] and level/game objective state are not serialized.",
    "all_behavior": "Short windows do not cover every map, bot action, objective or round lifecycle.",
}
SCORES = re.compile(r'^(-?\d+) (-?\d+) "(.*)"$')
MODE_DVAR = re.compile(r'^"g_gametype" is: "([a-z0-9_]+)\^7" default: "([a-z0-9_]+)\^7"\r?\n(?:latched: "([a-z0-9_]+)"\r?\n)?  Domain is any text\r?\n$')


def parse_native_reply(data, kind, challenge):
    prefix = b"\xff" * 4 + kind.encode("ascii") + b"\n"
    if type(data) is not bytes or len(data) > 16384 or not data.startswith(prefix) or b"\0" in data:
        raise ValueError("Malformed or oversized native browser/status response")
    lines = data[len(prefix):].decode("latin1").splitlines()
    if not lines or not lines[0].startswith("\\"):
        raise ValueError("Native response has no bounded info string")
    parts = lines[0][1:].split("\\")
    if len(parts) % 2 or any(not parts[index] for index in range(0, len(parts), 2)):
        raise ValueError("Native info string has an invalid key/value layout")
    pairs = list(zip(parts[::2], parts[1::2]))
    if len({key for key, _ in pairs}) != len(pairs):
        raise ValueError("Native info string duplicates a key")
    info = dict(pairs)
    if info.get("challenge") != challenge:
        raise ValueError("Native response challenge does not match this request")
    players = []
    for line in lines[1:]:
        if not line:
            continue
        match = SCORES.fullmatch(line)
        if kind != "statusResponse" or match is None or len(players) >= 16:
            raise ValueError("Malformed or excess actual native score lines")
        score, ping = int(match[1]), int(match[2])
        if not -2147483648 <= score <= 2147483647 or not -1 <= ping <= 9999:
            raise ValueError("Actual native score/ping is out of bounds")
        players.append({"score": score, "ping": ping, "name": match[3]})
    return {"info": info, "players": players, "raw_response": data.decode("latin1")}


def native_query(fixture, status=False):
    if fixture.server is None or not fixture.server.alive():
        raise RuntimeError("Exact owned server exited before native info/status query")
    challenge = "khzmatrix" + os.urandom(8).hex()
    kind = "statusResponse" if status else "infoResponse"
    until = min(fixture.deadline, time.monotonic() + 1.2)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(b"\xff" * 4 + (f"{'getstatus' if status else 'getinfo'} {challenge}").encode("ascii"),
                    ("127.0.0.1", fixture.server_port))
        while time.monotonic() < until:
            sock.settimeout(max(.01, until - time.monotonic()))
            data, source = sock.recvfrom(16385)
            if source != ("127.0.0.1", fixture.server_port):
                continue
            return parse_native_reply(data, kind, challenge)
    raise TimeoutError("Exact owned server did not return native info/status")


def parse_mode_dvar(data, expected):
    if type(data) is not bytes or len(data) > 4096 or b"\0" in data:
        raise ValueError("Native mode dvar print is malformed or oversized")
    text = data.decode("latin1")
    matched = MODE_DVAR.fullmatch(text)
    if matched is None:
        raise ValueError("Native mode readback has no complete current/latched/domain print")
    current, default, latched = matched.groups()
    if (latched or current) != expected:
        raise ValueError("Actual next-map g_gametype differs before the map command")
    return {"current": current, "default": default, "latched": latched,
            "effective_next_map": latched or current, "raw_response": text, "passed": True}


def native_mode_readback(fixture, expected):
    if fixture.server is None or not fixture.server.alive():
        raise RuntimeError("Exact owned server exited before native mode readback")
    delay = fixture.last_rcon + 1.1 - time.monotonic()
    if delay > 0:
        if time.monotonic() + delay >= fixture.deadline:
            raise TimeoutError("Budget exhausted before paced native mode readback")
        time.sleep(delay)
    fixture.last_rcon = time.monotonic()
    until = min(fixture.deadline, time.monotonic() + 1.2)
    output = bytearray()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(b"\xff" * 4 + b"rcon local-khz-benchmark g_gametype", ("127.0.0.1", fixture.server_port))
        while time.monotonic() < until:
            sock.settimeout(max(.01, until - time.monotonic()))
            data, source = sock.recvfrom(4097)
            if source != ("127.0.0.1", fixture.server_port):
                continue
            framing = b"\xff" * 4 + b"print\n"
            if not data.startswith(framing) or len(output) + len(data) - len(framing) > 4096:
                raise ValueError("Malformed or oversized exact-server native dvar reply")
            output.extend(data[len(framing):])
            if output.endswith(b"  Domain is any text\n") or output.endswith(b"  Domain is any text\r\n"):
                return parse_mode_dvar(bytes(output), expected)
    raise TimeoutError("No complete native current/latched mode print before map command")


def native_mode_errors(info, status, case):
    errors = []
    expected = (("gametype", case["gametype"]), ("mapname", case["map"]),
                ("protocol", "10002"), ("sv_maxclients", "16"), ("clients", str(case["bot_count"] + 1)))
    for key, value in expected:
        if info.get("info", {}).get(key) != value:
            errors.append("Actual native getinfo differs: " + key)
    actual = status.get("info", {})
    if actual.get("g_gametype") != case["gametype"] or actual.get("mapname") != case["map"]:
        errors.append("Actual native serverinfo gametype/map differs from the case")
    if len(status.get("players", [])) != case["bot_count"] + 1:
        errors.append("Actual stock status player score count differs")
    return errors


def inspection_errors(value, case, session, server_id):
    errors = []
    if type(value) is not dict:
        return ["Actual idle-VM inspection is not an object"]
    for name, expected in (("schema", 1), ("capacity", 16), ("bot_count", case["bot_count"]),
                           ("human_count", 1), ("session", session), ("server_id", server_id)):
        if not integer(value.get(name)) or value[name] != expected:
            errors.append("Actual idle-VM identity/population differs: " + name)
    if value.get("complete") is not True or value.get("reason") != "" or value.get("full_bot_acceptance") is not False:
        errors.append("Actual idle-VM inspection is incomplete, rejected or overclaims behavior")
    if not integer(value.get("tick"), 1):
        errors.append("Actual idle-VM inspection has no completed tick")
    slots = value.get("slots")
    if type(slots) is not list or len(slots) != case["bot_count"] + 1 or any(type(slot) is not dict or not integer(slot.get("slot"), 0, 15) for slot in slots):
        return errors + ["Actual native inspection has missing or invalid slots"]
    if len({slot["slot"] for slot in slots}) != len(slots) or sum(slot.get("bot") is True for slot in slots) != case["bot_count"] or sum(slot.get("bot") is False for slot in slots) != 1:
        errors.append("Actual bot/human slot namespace differs")
    teams = {team: 0 for team in range(4)}
    for slot in slots:
        if any(not integer(slot.get(name), 1) for name in ("generation", "entity_object", "pers_object", "last_spawn_time")):
            errors.append("Native admitted player lacks positive admission/script/spawn identity")
        team, state = slot.get("team"), slot.get("session_state")
        if not integer(team, 0, 3) or team not in ((0,) if case["gametype"] == "dm" else (1, 2)):
            errors.append("Actual native session team differs from stock rules")
        else:
            teams[team] += 1
        if not integer(state, 0, 2):
            errors.append("Actual native admission is in intermission or has invalid life state")
        elif state == 2 and (not integer(slot.get("health"), -2147483648, 0) or not integer(slot.get("last_spawn_time"), 1)):
            # Stock in_spawnSpectator can retain a dead player's playing team.
            # A live or never-spawned spectator is not an admission substitute.
            errors.append("Live or never-spawned spectator cannot substitute for a stock dead player")
        if not integer(slot.get("health"), -2147483648, 2147483647) or not integer(slot.get("weapon"), 0, 255):
            errors.append("Actual native health/weapon is invalid")
        if slot.get("bot") is True:
            if any(not integer(slot.get(name), 1) for name in ("bots_object", "skill_object", "command_ticks", "applied_tick")):
                errors.append("Actual native skill/control object is missing")
            skill = slot.get("skill", {})
            if type(skill) is not dict:
                errors.append("Actual typed VM skill is not an object")
                continue
            aim, initial, reaction = SKILL_TIMINGS[case["bot_difficulty"]]
            if not integer(skill.get("base"), 1, 7) or skill["base"] != SKILLS[case["bot_difficulty"]]:
                errors.append("Actual VM skill base differs from the stock-selected difficulty")
            actual_aim = skill.get("aim_time")
            if type(actual_aim) not in (int, float) or not math.isfinite(actual_aim) or abs(actual_aim - aim) > 1e-6:
                errors.append("Actual VM aim-time setting differs from selected difficulty")
            for name, expected in (("init_react_time", initial), ("reaction_time", reaction)):
                if not integer(skill.get(name)) or skill[name] != expected:
                    errors.append("Actual VM reaction setting differs: " + name)
    if case["gametype"] != "dm" and case["bot_count"] and (not teams[1] or not teams[2] or abs(teams[1] - teams[2]) > 1):
        errors.append("Actual populated native teams are not balanced within one player")
    return errors


def generation_transition_errors(before, after):
    errors = []
    old, new = before["sample"], after["sample"]
    a, b = old["stats"]["client_owner"], new["stats"]["client_owner"]
    if a["session"] == b["session"] or not integer(b["generation"], a["generation"] + 1) or old["state"]["server_id"] == new["state"]["server_id"]:
        errors.append("Actual native map/session/owner generation did not advance")
    prior = {slot["slot"]: slot for slot in before["inspection"]["slots"] if slot["bot"]}
    for slot in after["inspection"]["slots"]:
        if slot["bot"] and slot["slot"] in prior and slot["generation"] <= prior[slot["slot"]]["generation"]:
            errors.append("Old native bot control generation survived the loaded-map reset")
    return errors


def observation_evidence(before, after):
    a, b = before["server"]["native_bots"], after["server"]["native_bots"]
    deltas = {name: counter_delta(a, b, name) for name in
              ("command_ticks", "playing_ticks", "aim_ticks", "move_goals", "aim_goals", "respawn_pulses")}
    old_slots = {slot["slot"]: slot for slot in before["inspection"]["slots"]}
    changed = []
    for slot in after["inspection"]["slots"]:
        old = old_slots.get(slot["slot"])
        if old is None or (old["bot"], old["generation"]) != (slot["bot"], slot["generation"]):
            raise ValueError("Native admission changed inside the short observation window")
        if slot["bot"] and slot["command_ticks"] <= old["command_ticks"]:
            raise ValueError("An actual native bot stopped consuming selected commands")
        changed.append({"slot": slot["slot"], "bot": slot["bot"], "generation": slot["generation"],
                        "health_before": old["health"], "health_after": slot["health"],
                        "life_states": [old["session_state"], slot["session_state"]],
                        "spawn_times": [old["last_spawn_time"], slot["last_spawn_time"]]})
    return {"native_control_counter_deltas": deltas, "native_slot_observations": changed,
            "actual_score_lines_before": before["status"]["players"], "actual_score_lines_after": after["status"]["players"],
            "scope": "Actual native goal/control counters, health/life/spawn samples and G_GetClientScore status lines; no objective or combat attribution."}


class MatrixFixture(ManagedHostFixture):
    def __init__(self, project, output, port, case, budget):
        confined(project / "runtime/benchmarks" / f"client-{port}", project)
        super().__init__(project, output, port, budget=budget)
        self.case = case
        self.report.update(case=case, cases=[], probe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            diagnostic_gaps=dict(PENDING), complete_bot_acceptance=False,
            scope="Actual stock menu selections, native mode/admission/skill/team/selected-command evidence and same-child transitions; no timing or complete behavior gate.")

    def launch_shell(self, preserve_home=True):
        confined(self.home, self.project)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", self.port))
        if preserve_home:
            if self.home.exists():
                self.home.rename(self.saved_home); self.report["previous_home_preserved"] = True
            self.home_authorized = True
        elif not self.home_authorized or not self.home.is_dir():
            raise ValueError("Relaunch requires this fixture's existing isolated profile")
        arguments = [str(self.binary), "allowdupe", "+set", "net_port", str(self.port), "+set", "khz_benchmark", "1",
            "+set", "khz_test_controls", "1", "+set", "com_introPlayed", "1", "+set", "r_mode", "640x480",
            "+set", "r_customMode", "640x480", "+set", "r_fullscreen", "0"]
        if (self.project / "runtime/main/client.cfg").is_file():
            arguments += ["+exec", "client.cfg"]
        arguments += ["+set", "com_maxfps", "0", "+set", "r_vsync", "0", "+setqport", str(self.port),
                      "+set", "developer", "0", "+set", "khz_profile_scripts", "0"]
        self.client = subprocess.Popen(subprocess.list2cmdline(arguments), cwd=r"C:\Windows\Temp",
            env=os.environ | {"KHZ_BENCHMARK_RCON_PASSWORD": "local-khz-benchmark"})
        launch = {"pid": self.client.pid, "arguments": arguments, "cwd": r"C:\Windows\Temp", "auto_connect": False,
                  "same_existing_profile": not preserve_home}
        self.report.setdefault("launches", []).append(launch)
        self.report["owned_client_pid"] = self.client.pid
        until = min(self.deadline, time.monotonic() + 30)
        attempts = launch["startup_attempts"] = []
        while time.monotonic() < until:
            try:
                current = self.sample(); attempts.append(current)
                if leave_frontend_complete(current["stats"], current["state"], current["ui"], True):
                    windows = windows_for_pid(self.client.pid)
                    if not any(window["class"] == "CoD4" and window["visible"] and not window["minimized"] for window in windows):
                        raise ValueError("Created shell has no visible native game window")
                    launch.update(initial=current, windows=windows)
                    return current
            except (TimeoutError, OSError) as error:
                launch["last_query_error"] = str(error)
            time.sleep(.1)
        raise TimeoutError("Created native shell did not reach the visible stock main menu")

    def quit_shell(self):
        self.return_to_main()
        sample = self.sample()
        if not leave_frontend_complete(sample["stats"], sample["state"], sample["ui"], self.server is None or not self.server.alive()):
            raise ValueError("Profile relaunch requires a disconnected stock frontend")
        reply = self.call("khz_ui quit")
        if reply.get("result") != "queued":
            raise ValueError("Native shell Quit was not queued")
        self.client.wait(timeout=8)
        record = {"pid": self.client.pid, "reply": reply, "returncode": self.client.returncode}
        self.report.setdefault("normal_shell_exits", []).append(record)
        if self.client.returncode != 0:
            raise ValueError("Created profile shell did not exit normally")
        self.client = None

    def return_to_main(self):
        if self.server is not None:
            raise ValueError("Menu-only navigation cannot bypass owned-server Leave")
        current = self.sample()
        if current["state"].get("connection") != 0 or current["ui"].get("host", {}).get("owned") is not False:
            raise ValueError("Menu-only navigation requires actual disconnected/unowned native state")
        initial = self.report["launches"][-1].get("initial", {})
        name = initial.get("ui", {}).get("top_menu")
        if name not in ("main", "main_text"):
            raise ValueError("This created shell has no observed stock startup menu")
        opened = self.call("khz_ui menu " + name)
        if opened.get("result") != "opened" or opened.get("top_menu") != name:
            raise ValueError("Native stock startup menu navigation failed")
        actual = self.sample()
        self.report.setdefault("menu_only_returns", []).append({"before": current, "open": opened, "after": actual})
        if not leave_frontend_complete(actual["stats"], actual["state"], actual["ui"], True):
            raise ValueError("Observed stock main menu is not a healthy disconnected native frontend")

    def native_server(self, require_ready=True):
        value = self.rcon("khz_json")
        if value.get("process_id") != self.server.pid:
            raise RuntimeError("RCON endpoint differs from the exact retained child")
        if require_ready:
            require_no_errors(bot_server_errors(value, self.port, self.case["bot_count"], self.case["bot_difficulty"]))
        if value.get("plugins", {}).get("loaded") != 0:
            raise ValueError("Bot matrix cannot contain loaded plugins")
        return value

    def wait_case(self, label, timeout=30):
        stage = {"name": label, "samples": [], "passed": False}
        self.report.setdefault("readiness", []).append(stage)
        until = min(self.deadline, time.monotonic() + timeout)
        while time.monotonic() < until:
            if self.server is not None and not self.server.alive():
                raise RuntimeError("Exact owned server exited during native case readiness")
            try:
                current = self.sample()
                host = current["ui"].get("host", {})
                if self.server is None and host.get("owned") is True and host.get("authenticated") is True and integer(host.get("child_pid"), 1):
                    if not integer(host.get("port"), 1, 65535):
                        raise ValueError("Owned child has no bounded native port")
                    self.server = OwnedServer(host["child_pid"], self.server_binary)
                    self.server_port = host["port"]
                    self.server_home = self.project / "runtime/benchmarks" / f"server-{self.server_port}"
                    confined(self.server_home, self.project)
                    self.report["owned_server"] = {"pid": self.server.pid, "path": self.server.path, "port": self.server_port}
                require_no_errors(match_errors(current, self.case, self.client.pid, self.server.pid if self.server else None))
                value = {"sample": current, "server": self.native_server(),
                         "inspection": self.rcon("khz_bot_inspect", "KHZ_BOT_INSPECTION"),
                         "info": native_query(self), "status": native_query(self, True)}
                stage["samples"].append(value)
                require_no_errors(inspection_errors(value["inspection"], self.case,
                    current["stats"]["client_owner"]["session"], current["state"]["server_id"]))
                require_no_errors(native_mode_errors(value["info"], value["status"], self.case))
                stage.update(passed=True, recovered=value)
                return value
            except (TimeoutError, OSError, ValueError) as error:
                stage.setdefault("errors", []).append({"elapsed": time.monotonic() - self.started, "error": str(error)})
            time.sleep(.1)
        raise TimeoutError("Bounded actual case did not become ready: " + str(stage.get("errors", [])[-1:]))

    def close(self):
        # The archived shared fixture expects a child-server handle. The
        # separate menu-only persistence action deliberately never creates one.
        menu_only = self.server is None
        passed_before_cleanup = self.report["passed"]
        if menu_only and self.client and self.client.poll() is None:
            try:
                self.return_to_main()
            except (ValueError, TimeoutError, OSError) as error:
                self.report["errors"].append("Menu-only cleanup navigation: " + str(error))
        super().close()
        if menu_only:
            cleanup = self.report["cleanup"]
            proof = cleanup.get("leave_frontend_ready") is True or bool(self.report.get("normal_shell_exits")) and self.client is None
            cleanup["no_child_was_created"] = True
            cleanup["menu_only_frontend_verified"] = proof
            self.report["passed"] = bool(passed_before_cleanup and proof and
                not self.report["errors"] and cleanup["created_client_exited"])
            (self.output / "result.json").write_text(json.dumps(self.report, indent=2), encoding="utf8")


def run_case(fixture, index, first, previous=None):
    entry = {"case": dict(fixture.case), "passed": False}
    fixture.report["cases"].append(entry)
    entry["before"] = first
    if previous is not None:
        require_no_errors(generation_transition_errors(previous, first))
        entry["native_generation_reset_verified"] = True
    observation_started = time.monotonic()
    fixture.report["actual_vm_team_inspection"] = {"passed": True, "samples": [first["inspection"]]}
    capture(fixture, index)
    tape = fixture.report["command_tapes"][-1]
    # The imported helper has already hashed, retained and validated every
    # serialized record. Drain retires those contents, never replaces them.
    tape["drain"] = fixture.rcon("khz_server_capture drain", "KHZ_SERVER_CAPTURE")
    require_no_errors(tape_status_errors(tape["drain"], True))
    if any(tape["drain"].get(name) != tape["status"].get(name) for name in
           ("fixture", "session", "server_id", "recorded", "first_tick", "last_tick")):
        raise ValueError("Draining a validated retained tape changed its native identity")
    final = entry["after"] = fixture.wait_case("after retained tape " + fixture.case["name"], timeout=10)
    entry["continuity"] = continuity(first["sample"], final["sample"])
    entry["server_continuity"] = server_continuity(first["server"], final["server"], fixture.port)
    entry["client_debt"] = debt_bounds([first["sample"]["stats"], final["sample"]["stats"]])
    entry["server_debt"] = debt_bounds([first["server"], final["server"]])
    entry["observations"] = observation_evidence(first, final)
    entry["tape"] = {"number": index, "retained_file": tape["retained_file"], "status": tape["status"], "drain": tape["drain"]}
    entry["observed_window_seconds"] = time.monotonic() - observation_started
    entry["short_window_target_seconds"] = 10
    entry["objective_completion_verified"] = entry["combat_outcomes_verified"] = entry["reaction_behavior_verified"] = False
    entry["passed"] = True
    return final


def run_host(fixture, modes, ready_timeout):
    fixture.launch_shell()
    catalog = fixture.report["actual_loaded_game_types"] = fixture.call("khz_ui list")
    actual_types = catalog.get("game_types")
    if catalog.get("game_types_valid") is not True or type(actual_types) is not list or any(mode not in actual_types for mode in MODES):
        raise ValueError("The actual loaded stock game-type catalog does not contain all six requested modes")
    for mode in MODES:
        prepare_menu(fixture, "settings_quick_" + mode, fixture.case["bot_count"], fixture.case["bot_difficulty"])
    before = fixture.call("khz_ui status")
    if before["ui"]["bot_count"] != fixture.case["bot_count"] or before["ui"]["bot_difficulty"] != fixture.case["bot_difficulty"]:
        raise ValueError("Actual archived stock selections differ before native StartServer")
    fixture.report["menu_settings_before_host"] = before
    reply = fixture.call(f"khz_ui host {fixture.case['map']} {fixture.case['gametype']} {fixture.case['bot_count']} {fixture.case['bot_difficulty']}")
    fixture.report["host_request"] = reply
    if reply.get("result") != "start requested":
        raise ValueError("Actual stock typed mode/map StartServer was rejected")
    previous = None
    for index, mode in enumerate(modes, 1):
        if index > 1:
            previous = final
            fixture.case = {**fixture.case, "name": DIFFICULTIES[fixture.case["bot_difficulty"]].lower() + "_" + mode,
                            "gametype": mode}
            transition = {"to": dict(fixture.case), "child_pid": fixture.server.pid}
            fixture.report.setdefault("mode_transitions", []).append(transition)
            # Fire is only administration enqueue; independent actual native
            # getinfo/serverinfo/VM observations are required before passing.
            fixture.rcon("g_gametype " + mode, fire=True)
            transition["native_rcon"] = "g_gametype " + mode
            transition["actual_next_map_mode_readback"] = native_mode_readback(fixture, mode)
            transition["map_reply"] = fixture.call("khz_ui server map " + fixture.case["map"])
            if transition["map_reply"].get("result") != "server command queued":
                raise ValueError("Native owned-server map transition was not queued")
        first = fixture.wait_case("mode transition " + mode, ready_timeout)
        saved_deadline = fixture.deadline
        fixture.deadline = min(saved_deadline, time.monotonic() + 10)
        try:
            final = run_case(fixture, index, first, previous)
        finally:
            fixture.deadline = saved_deadline
    fixture.leave()
    fixture.report["passed"] = all(case["passed"] for case in fixture.report["cases"])


def run_persistence(fixture):
    fixture.launch_shell()
    fixture.report["profile_persistence"] = records = []
    for count, difficulty in ((0, 3), (15, 0)):
        prepare_menu(fixture, "settings_quick_dm", count, difficulty)
        selected = archived_config(fixture)
        profile = (fixture.home / "players/profiles/active.txt").read_text(encoding="latin1").strip()
        entry = {"count": count, "difficulty": difficulty, "profile": profile,
                 "selected_archive": archive_evidence(selected, ("ui_khz_botCount", "ui_khz_botDifficulty")), "passed": False}
        records.append(entry)
        fixture.quit_shell()
        # No StartServer, host adapter, menu key or bot dvar setter occurs
        # between this retained archive and the next observed native startup.
        entry["archive_after_quit"] = archive_evidence(archived_config(fixture), ("ui_khz_botCount", "ui_khz_botDifficulty"))
        actual = fixture.launch_shell(preserve_home=False)
        active = (fixture.home / "players/profiles/active.txt").read_text(encoding="latin1").strip()
        entry["relaunch_before_any_selection"] = actual
        entry["relaunch_archive"] = archive_evidence(archived_config(fixture), ("ui_khz_botCount", "ui_khz_botDifficulty"))
        if active != profile or actual["ui"]["ui"].get("bot_count") != count or actual["ui"]["ui"].get("bot_difficulty") != difficulty:
            raise ValueError("Actual same-profile startup did not restore the saved stock bot selections")
        if entry["relaunch_archive"]["settings"] != entry["selected_archive"]["settings"]:
            raise ValueError("Same-profile archived bot settings changed across native Quit/relaunch")
        entry["passed"] = True
    fixture.report["passed"] = True


def supplement_provenance(project, archive):
    proof = archived_provenance(project, archive)
    helper = "scripts/native_bot_admission_probe.py"
    digest = hashlib.sha256((project / helper).read_bytes()).hexdigest()
    archived = json.loads(archive.read_text(encoding="utf-8-sig"))
    if archived.get("source_sha256", {}).get(helper) != digest:
        raise ValueError("Imported admission provenance helper differs from the frozen archive")
    proof["imported_helper_sha256"][helper] = digest
    own = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    expected = archived.get("source_sha256", {}).get("scripts/native_bot_matrix_probe.py")
    proof.update(new_harness_path=str(Path(__file__).resolve()), new_harness_sha256=own,
                 new_harness_in_frozen_archive=expected is not None, archived_harness_sha256=expected,
                 new_harness_matches_frozen_archive=(own == expected) if expected is not None else None)
    return proof


def self_test():
    case = {"map": "mp_crash", "gametype": "dm", "bot_count": 15, "bot_difficulty": 2}
    slots = [{"slot": index, "bot": index != 0, "generation": 3, "entity_object": index + 1,
              "pers_object": index + 1, "bots_object": 32, "skill_object": 33,
              "last_spawn_time": 200, "team": 0, "session_state": 0, "health": 100,
              "weapon": 27, "command_ticks": 10, "applied_tick": 10,
              "skill": {"base": 5, "aim_time": .25, "init_react_time": 500, "reaction_time": 300}}
             for index in range(16)]
    value = {"schema": 1, "capacity": 16, "bot_count": 15, "human_count": 1, "session": 2,
             "server_id": 3, "tick": 10, "complete": True, "reason": "", "full_bot_acceptance": False, "slots": slots}
    assert not inspection_errors(value, case, 2, 3)
    import copy
    dead = copy.deepcopy(value); dead["slots"][1].update(session_state=2, health=0)
    assert not inspection_errors(dead, case, 2, 3)
    for field, wrong in (("health", 100), ("last_spawn_time", 0), ("team", 3)):
        invalid = copy.deepcopy(dead); invalid["slots"][1][field] = wrong
        assert inspection_errors(invalid, case, 2, 3)
    team_case = {**case, "gametype": "sd"}
    balanced = copy.deepcopy(value)
    for index, slot in enumerate(balanced["slots"]):
        slot["team"] = 1 if index < 8 else 2
    assert not inspection_errors(balanced, team_case, 2, 3)
    unbalanced = copy.deepcopy(balanced); unbalanced["slots"][8]["team"] = 1
    assert inspection_errors(unbalanced, team_case, 2, 3)
    for name, wrong in (("base", 3), ("aim_time", .4), ("init_react_time", 750), ("reaction_time", False)):
        invalid = copy.deepcopy(value); invalid["slots"][1]["skill"][name] = wrong
        assert inspection_errors(invalid, case, 2, 3)
    for difficulty in range(4):
        configured = copy.deepcopy(value)
        aim, initial, reaction = SKILL_TIMINGS[difficulty]
        for slot in configured["slots"]:
            slot["skill"] = {"base": SKILLS[difficulty], "aim_time": aim,
                             "init_react_time": initial, "reaction_time": reaction}
        for mode in MODES:
            for index, slot in enumerate(configured["slots"]):
                slot["team"] = 0 if mode == "dm" else 1 if index < 8 else 2
            assert not inspection_errors(configured, {**case, "gametype": mode, "bot_difficulty": difficulty}, 2, 3)
    for field in ("complete", "session", "server_id", "slots"):
        missing = copy.deepcopy(value); del missing[field]
        assert inspection_errors(missing, case, 2, 3)
    invalid = copy.deepcopy(value); invalid["slots"][1]["skill"] = None
    assert inspection_errors(invalid, case, 2, 3)
    invalid = copy.deepcopy(value); invalid["slots"][1]["slot"] = 0
    assert inspection_errors(invalid, case, 2, 3)
    empty = copy.deepcopy(value); empty["slots"] = [empty["slots"][0]]; empty["bot_count"] = 0
    assert not inspection_errors(empty, {**case, "bot_count": 0}, 2, 3)
    raw = b"\xff" * 4 + b'infoResponse\n\\challenge\\abc\\gametype\\dm\n'
    assert parse_native_reply(raw, "infoResponse", "abc")["info"]["gametype"] == "dm"
    for invalid in (raw.replace(b"abc", b"other"), raw + b"\0", raw.replace(b"\\gametype\\dm", b"\\gametype\\dm\\gametype\\war")):
        try:
            parse_native_reply(invalid, "infoResponse", "abc")
        except ValueError:
            pass
        else:
            raise AssertionError("Malformed/foreign native status passed")
    scores = b"\xff" * 4 + b'statusResponse\n\\challenge\\abc\\g_gametype\\dm\\mapname\\mp_crash\n10 0 "bot"\n'
    assert parse_native_reply(scores, "statusResponse", "abc")["players"] == [{"score": 10, "ping": 0, "name": "bot"}]
    try:
        parse_native_reply(scores.replace(b'10 0 "bot"', b'NaN 0 "bot"'), "statusResponse", "abc")
    except ValueError:
        pass
    else:
        raise AssertionError("Corrupt native score passed")
    latched = b'"g_gametype" is: "dm^7" default: "war^7"\nlatched: "sd"\n  Domain is any text\n'
    assert parse_mode_dvar(latched, "sd")["effective_next_map"] == "sd"
    current = b'"g_gametype" is: "dm^7" default: "war^7"\n  Domain is any text\n'
    assert parse_mode_dvar(current, "dm")["latched"] is None
    for bad, expected in ((latched, "dm"), (current, "sd"), (latched[:-1], "sd"),
                          (latched + b"\0", "sd"), (latched.replace(b"latched:", b"queued:"), "sd")):
        try:
            parse_mode_dvar(bad, expected)
        except ValueError:
            pass
        else:
            raise AssertionError("Missing/stale/partial native mode completion passed")
    old = {"sample": {"stats": {"client_owner": {"session": 2, "generation": 4}}, "state": {"server_id": 3}}, "inspection": value}
    new = copy.deepcopy(old)
    new["sample"]["stats"]["client_owner"].update(session=5, generation=6)
    new["sample"]["state"]["server_id"] = 7
    for slot in new["inspection"]["slots"]:
        slot["generation"] += 1
    assert not generation_transition_errors(old, new)
    for mutate in (lambda sample: sample["sample"]["stats"]["client_owner"].update(session=2),
                   lambda sample: sample["sample"]["stats"]["client_owner"].update(generation=4),
                   lambda sample: sample["sample"]["state"].update(server_id=3),
                   lambda sample: sample["inspection"]["slots"][1].update(generation=3)):
        invalid = copy.deepcopy(new); mutate(invalid)
        assert generation_transition_errors(old, invalid)
    assert debt_bounds([{"debt": 0}, {"debt": 8}])["debt_pass"] is True
    try:
        debt_bounds([{"debt": 0}, {"debt": 9}])
    except ValueError:
        pass
    else:
        raise AssertionError("Growing scheduler debt passed")
    cells = {(difficulty, mode) for difficulty in range(4) for mode in MODES}
    assert len(cells) == 24
    from types import SimpleNamespace
    menu_sample = {"stats": {"presentation_state": {"renderer_started": True, "screen_initialized": True, "engine_error": False}},
                   "state": {"connection": 0}, "ui": {"host": {"owned": False}, "top_menu": "settings_quick_dm"}}
    def menu_mock(current=menu_sample, name="main_text", reply="opened"):
        calls, samples = [], [copy.deepcopy(current), {**copy.deepcopy(current), "ui": {"host": {"owned": False}, "top_menu": name}}]
        return SimpleNamespace(server=None, report={"launches": [{"initial": {"ui": {"top_menu": name}}}]},
            sample=lambda: samples.pop(0), call=lambda command: calls.append(command) or {"result": reply, "top_menu": name}), calls
    shell, calls = menu_mock(); MatrixFixture.return_to_main(shell)
    assert calls == ["khz_ui menu main_text"] and shell.report["menu_only_returns"][0]["before"]["ui"]["top_menu"] == "settings_quick_dm"
    connected = copy.deepcopy(menu_sample); connected["state"]["connection"] = 9
    for shell in (menu_mock(connected)[0], menu_mock(name="unobserved-menu")[0], menu_mock(reply="open failed")[0]):
        try:
            MatrixFixture.return_to_main(shell)
        except ValueError:
            pass
        else:
            raise AssertionError("Invalid menu-only navigation proof passed")
    print("Bot matrix validators passed: 24 distinct cells, all four typed skills, teams/death/empty-population guards, native reply corruption, generation retirement and debt bounds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--action", choices=("matrix", "persistence", "all"), default="matrix")
    parser.add_argument("--build-archive", type=Path)
    parser.add_argument("--client-port", type=int, default=28991)
    parser.add_argument("--map", choices=("mp_killhouse", "mp_crash"), default="mp_crash")
    parser.add_argument("--difficulty", type=int, choices=range(4), help="Scoped one-host subset; omission runs all24difficulty/mode cells")
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--readiness-timeout", type=float, default=30)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(); return 0
    if os.name != "nt" or args.build_archive is None or not 1024 <= args.client_port <= 65535 or not 5 <= args.readiness_timeout <= 30 or len(set(args.modes)) != len(args.modes):
        parser.error("Windows, explicit frozen archive, distinct modes, port1024..65535 and readiness5..30s are required")
    project = Path(__file__).resolve().parents[1]
    archive = confined(args.build_archive.absolute(), project)
    output = confined((args.output or project / "results" / ("native-bot-matrix-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))).absolute(), project / "results")
    if output.exists() or output.parent != project / "results":
        parser.error("Output must be a new direct child of project/results")
    output.mkdir()
    report = {"schema": 1, "passed": False, "full_24_case_matrix_pass": False, "complete_bot_acceptance": False,
              "errors": [], "runs": [], "pending": dict(PENDING), "scope": "Short actual native skill/team/control/settings observations; all objective/combat/reaction/full-world/timing gates remain separate."}
    before = after = None
    try:
        report["provenance_before"] = before = supplement_provenance(project, archive)
        report["interactive_preferences_before"] = interactive_preferences(project)
        difficulties = range(4) if args.difficulty is None else (args.difficulty,)
        plans = [(DIFFICULTIES[difficulty].lower(), difficulty, 15, args.modes) for difficulty in difficulties] if args.action != "persistence" else []
        if args.action != "persistence":
            plans += [("desired-zero", 1, 0, ["dm"])]
        if args.action in ("all", "persistence"):
            plans += [("same-profile-persistence", 1, 0, [])]
        for name, difficulty, count, modes in plans:
            run_output = output / name; run_output.mkdir()
            case = {"name": name + "_" + (modes[0] if modes else "dm"), "map": args.map,
                    "gametype": modes[0] if modes else "dm", "bot_count": count, "bot_difficulty": difficulty}
            budget = max(90, len(modes) * (args.readiness_timeout + 15) + 50) if modes else 80
            fixture = MatrixFixture(project, run_output, args.client_port, case, budget)
            try:
                if modes:
                    run_host(fixture, modes, args.readiness_timeout)
                else:
                    run_persistence(fixture)
            except Exception as error:
                fixture.report["errors"].append(str(error)); fixture.report["passed"] = False
            finally:
                fixture.close()
            report["runs"].append({"name": name, "difficulty": difficulty, "bots": count,
                "case_count": len(fixture.report["cases"]), "result": str(run_output / "result.json"),
                "passed": fixture.report["passed"], "errors": fixture.report["errors"]})
            if not fixture.report["passed"]:
                break
        report["passed"] = len(report["runs"]) == len(plans) and all(run["passed"] for run in report["runs"])
        report["full_24_case_matrix_pass"] = (report["passed"] and args.action != "persistence" and args.difficulty is None
            and set(args.modes) == set(MODES) and sum(run["case_count"] for run in report["runs"] if run["bots"] == 15) == 24)
    except Exception as error:
        report["errors"].append(str(error))
    finally:
        try:
            report["provenance_after_cleanup"] = after = supplement_provenance(project, archive)
            report["provenance_unchanged"] = before is not None and before == after
            report["interactive_preferences_after"] = interactive_preferences(project)
            report["interactive_preferences_unchanged"] = report.get("interactive_preferences_before") == report["interactive_preferences_after"]
            if not report["provenance_unchanged"] or not report["interactive_preferences_unchanged"]:
                report["errors"].append("Frozen binary/assets/helpers or interactive preferences changed")
        except Exception as error:
            report["errors"].append("Post-cleanup provenance: " + str(error))
        if report["errors"]:
            report["passed"] = report["full_24_case_matrix_pass"] = False
        (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    print(json.dumps({"passed": report["passed"], "full_24_case_matrix_pass": report["full_24_case_matrix_pass"],
                      "runs": len(report["runs"]), "output": str(output)}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
