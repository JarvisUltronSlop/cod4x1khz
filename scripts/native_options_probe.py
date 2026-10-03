"""Bounded native Options diagnostic with per-command owner observations.

The prefix mode follows the successful categories immediately preceding the
Build 69 Look-menu barrier failure. Observations alter command timing: a pass
here does not attest to complete Options or gameplay acceptance.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time

from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import (OPTIONS_CATEGORIES, archived_config, exercise_stock_category, interactive_preferences,
                              one_control, stock_action, stock_inventory, wait_recovery)
from native_lifecycle_probe import counter_delta, native_map_matches, server_continuity


PREFIX = ("main_options", "options_graphics_texture", "options_voice", "options_game", "options_multi")


def observed_action(command):
    return command.startswith(("khz_ui menu ", "khz_ui activate ", "khz_ui key "))


class ObservedFixture(ManagedHostFixture):
    observe = False

    def telemetry(self):
        # Status UI probes acquire the very barrier under investigation. Query
        # its independent telemetry endpoints instead, without a nested probe.
        result = {"elapsed": time.monotonic() - self.started}
        for name, command, marker in (("stats", "khz_stats", "KHZ_JSON"), ("state", "khz_state", "KHZ_STATE")):
            try:
                value = super().call(command, marker, timeout=.3)
                if name == "stats" and value.get("process_id") != self.client.pid:
                    raise ValueError("Diagnostic endpoint is not the created client")
                result[name] = value
            except (TimeoutError, OSError, ValueError, RuntimeError) as error:
                result[name + "_error"] = str(error)
        return result

    def call(self, command, marker="KHZ_UI", timeout=1.2):
        if not self.observe or not observed_action(command):
            return super().call(command, marker, timeout)
        attempt = {"command": command, "before": self.telemetry(), "passed": False}
        self.report.setdefault("command_attempts", []).append(attempt)
        started = time.monotonic()
        try:
            reply = super().call(command, marker, timeout)
            attempt["reply"] = reply
            attempt["received"] = True
            attempt["passed"] = reply.get("result") in ("opened", "stock action executed", "stock key dispatched")
            return reply
        except Exception as error:
            attempt["error"] = str(error)
            self.report.setdefault("first_unanswered_command", command)
            raise
        finally:
            attempt["query_seconds"] = time.monotonic() - started
            attempt["after"] = self.telemetry()


def stress_continuity(before, after, map_name):
    a, b = before.get("stats", {}), after.get("stats", {})
    original = a.get("client_owner", {})
    for sample in (before, after):
        stats, state = sample.get("stats", {}), sample.get("state", {})
        owner, screen = stats.get("client_owner", {}), stats.get("presentation_state", {})
        if (stats.get("process_id") != a.get("process_id") or type(a.get("process_id")) is not int or
                any(owner.get(key) != original.get(key) or type(original.get(key)) is not int or original[key] <= 0
                    for key in ("session", "generation")) or owner.get("phase") != "running" or
                owner.get("active") is not True or owner.get("private_domains") is not True or
                owner.get("native_connection") != 9 or owner.get("native_cgame_initialized") is not True or
                state.get("connection") != 9 or not native_map_matches(state.get("map"), map_name) or
                screen.get("renderer_started") is not True or screen.get("engine_error") is not False):
            raise ValueError("Real Options stress changed or stopped the native owner/map/renderer lifetime")
        for key in ("snapshot_backlog", "event_backlog", "queue_failures"):
            if type(owner.get(key)) is not int or owner[key] < 0:
                raise ValueError("Actual owner queue telemetry is missing during Options stress")
    counters = {name: counter_delta(a, b, name) for name in
                ("ack_input", "commands_sent", "snapshots_received", "prediction_ticks", "gpu_presents")}
    owner_counters = {name: counter_delta(original, b["client_owner"], name) for name in ("ticks", "snapshots", "pose_ticks")}
    if not all(value > 0 for value in (*counters.values(), *owner_counters.values())):
        raise ValueError("Authoritative acknowledgements, native gameplay, snapshots, pose or GPU progress stopped during Options stress")
    if counter_delta(original, b["client_owner"], "queue_failures"):
        raise ValueError("Native owner reported a queue failure during Options stress")
    raw_before, raw_after = a.get("raw_input", {}), b.get("raw_input", {})
    if counter_delta(raw_before, raw_after, "dropped"):
        raise ValueError("Real input producer dropped a queued event during Options stress")
    for raw in (raw_before, raw_after):
        if type(raw.get("queued")) is not int or not 0 <= raw["queued"] <= 4096:
            raise ValueError("Actual bounded input queue telemetry is missing")
    if raw_after["queued"] > max(8, raw_before["queued"] + 2):
        raise ValueError("The actual input queue grew across a completed real Options round")
    for stats in (a, b):
        if type(stats.get("debt")) is not int or stats["debt"] < 0:
            raise ValueError("Actual client scheduler debt is missing")
    debt_pass = b["debt"] <= max(8, a["debt"] + 2)
    queues = {}
    for name in ("snapshot_backlog", "event_backlog"):
        start, finish = original[name], b["client_owner"][name]
        queues[name] = {"start": start, "end": finish, "passed": finish <= max(8, start + 2)}
    if not debt_pass or not all(value["passed"] for value in queues.values()):
        raise ValueError("Native queue or scheduler debt grew across a completed real Options round")
    return {"passed": True, "session": original["session"], "generation": original["generation"],
            "client_counter_deltas": counters, "owner_counter_deltas": owner_counters, "queues": queues,
            "debt": {"start": a["debt"], "end": b["debt"], "passed": debt_pass},
            "raw_queued": {"start": raw_before.get("queued"), "end": raw_after.get("queued")}}


def preserved_preferences(before, after):
    # Actual persistent graphics/audio/input selections matter here. Stock
    # menu staging dvars can legitimately initialize while merely opening UI.
    names = {key for key in before["settings"] if key.startswith(("r_", "snd_", "m_")) or
             key in ("cg_fovScale", "com_maxfps", "sensitivity")}
    if not {"cg_fovScale", "com_maxfps", "r_vsync", "snd_khz"} <= names:
        raise ValueError("The actual archived graphics/audio/FOV selections are missing")
    changes = {key: {"before": before["settings"].get(key), "after": after["settings"].get(key)}
               for key in sorted(names) if before["settings"].get(key) != after["settings"].get(key)}
    return {"passed": not changes and before["bindings"] == after["bindings"], "protected_settings": sorted(names),
            "changes": changes, "bindings_unchanged": before["bindings"] == after["bindings"]}


def run_stress(fixture, map_name, rounds, seconds):
    proof = fixture.report["control_barrier_stress"] = {
        "passed": False, "requested_rounds": rounds, "maximum_seconds": seconds, "categories": list(OPTIONS_CATEGORIES),
        "scope": "Repeated real compiled menu open/back actions with observed native ownership/admission. No setting changes or Apply actions.",
        "rounds": []}
    initial_archive = archived_config(fixture)
    first_server = fixture.native_server()
    cache = {}
    started = time.monotonic()
    until = min(fixture.deadline, started + seconds)
    for index in range(rounds):
        round_proof = {"round": index + 1, "passed": False, "categories": [], "before": fixture.telemetry()}
        proof["rounds"].append(round_proof)
        for name in OPTIONS_CATEGORIES:
            if time.monotonic() >= until:
                raise TimeoutError("Bounded real Options stress time expired before all requested categories completed")
            category = {"name": name, "passed": False}
            round_proof["categories"].append(category)
            try:
                category["open"] = fixture.call("khz_ui menu " + name)
                if category["open"].get("result") != "opened" or category["open"].get("top_menu") != name:
                    raise ValueError("Actual native stress menu did not become active: " + name)
                if name not in cache:
                    cache[name] = one_control(stock_inventory(fixture, name), name="back", kind=1)
                category["back"] = stock_action(fixture, name, cache[name])
                category["passed"] = True
            except Exception as error:
                category["error"] = str(error)
                fixture.report["first_failed_category"] = name
                raise
        round_proof["after"] = fixture.telemetry()
        round_proof["continuity"] = stress_continuity(round_proof["before"], round_proof["after"], map_name)
        round_proof["passed"] = True
    proof["elapsed_seconds"] = time.monotonic() - started
    proof["server_continuity"] = server_continuity(first_server, fixture.native_server(), fixture.port)
    proof["preferences"] = preserved_preferences(initial_archive, archived_config(fixture))
    if not proof["preferences"]["passed"]:
        raise ValueError("Open/back stress unexpectedly changed real archived selections or bindings")
    proof["passed"] = len(proof["rounds"]) == rounds and all(entry["passed"] for entry in proof["rounds"])


def run(fixture, prefix=False, map_name="mp_crash", stress_rounds=0, stress_seconds=10):
    before = fixture.start()
    fixture.report.update(scope=__doc__, options_prefix_requested=prefix, target_menu="options_look",
                          target_map=map_name, sustained_timing_verified=False)
    if map_name != "mp_killhouse":
        fixture.report["map_request"] = fixture.call("khz_ui server map " + map_name)
        if fixture.report["map_request"].get("result") != "server command queued":
            raise ValueError("The owned native map command was rejected")
        wait_recovery(fixture, "Options diagnostic Crash", map_name, before, changed_map=True, timeout=20)
    fixture.observe = True
    report = fixture.report["stock_options"] = {"passed": False, "categories": {}, "apply": []}
    report["mode"] = "open_back_stress" if stress_rounds else "setting_diagnostic"
    categories = () if stress_rounds else (PREFIX + ("options_look",) if prefix else ("options_look",))
    report["attempt_order"] = categories
    for name in categories:
        category = report["categories"][name] = {"passed": False, "actions": []}
        try:
            # Scale 1 is the untouched stock profile default in this fresh
            # fixture. The diagnostic does not recalibrate FOV or mutate it.
            exercise_stock_category(fixture, report, name, map_name, 1., category)
        except Exception as error:
            category["error"] = str(error)
            fixture.report["first_failed_category"] = name
            raise
    if stress_rounds:
        run_stress(fixture, map_name, stress_rounds, stress_seconds)
    fixture.observe = False
    first = fixture.sample()
    fixture.report["post_options_recovery"] = wait_recovery(fixture, "Options diagnostic gameplay", map_name, first, timeout=5)
    report["passed"] = True
    fixture.leave()
    fixture.report["passed"] = True


def self_test():
    for text in ("khz_ui menu options_look", "khz_ui activate main_options 8", "khz_ui key options_look 74 157"):
        assert observed_action(text)
    for text in ("khz_ui status", "khz_ui inspect options_look 0", "khz_stats", "khz_state"):
        assert not observed_action(text)
    # An unanswered command must remain journalled, together with both
    # observations, even when the native handler returns no JSON at all.
    fixture = ObservedFixture.__new__(ObservedFixture)
    fixture.observe, fixture.report = True, {}
    fixture.telemetry = lambda: {"stats_error": "main handler waiting"}
    original = ManagedHostFixture.call
    try:
        def timeout(*args, **kwargs):
            raise TimeoutError("bounded unanswered native command")
        ManagedHostFixture.call = timeout
        try:
            fixture.call("khz_ui menu options_look")
        except TimeoutError:
            pass
        else:
            raise AssertionError("Unanswered native command accepted")
    finally:
        ManagedHostFixture.call = original
    attempt = fixture.report["command_attempts"][0]
    assert fixture.report["first_unanswered_command"] == "khz_ui menu options_look"
    assert attempt["passed"] is False and "error" in attempt and attempt["after"]["stats_error"]
    owner = {"phase": "running", "active": True, "private_domains": True, "session": 1, "generation": 2,
             "native_connection": 9, "native_cgame_initialized": True, "ticks": 10, "snapshots": 10, "pose_ticks": 10,
             "snapshot_backlog": 0, "event_backlog": 0, "queue_failures": 0}
    stats = {"client_owner": owner, "process_id": 7, "presentation_state": {"renderer_started": True, "engine_error": False},
             "raw_input": {"dropped": 0, "queued": 0}, "debt": 0}
    for name in ("ack_input", "commands_sent", "snapshots_received", "prediction_ticks", "gpu_presents"):
        stats[name] = 10
    sample = {"stats": stats, "state": {"connection": 9, "map": "mp_crash"}}
    final = json.loads(json.dumps(sample))
    for name in ("ack_input", "commands_sent", "snapshots_received", "prediction_ticks", "gpu_presents"):
        final["stats"][name] += 10
    for name in ("ticks", "snapshots", "pose_ticks"):
        final["stats"]["client_owner"][name] += 10
    assert stress_continuity(sample, final, "mp_crash")["passed"]
    invalid = []
    for key, value in (("generation", 3), ("phase", "parked"), ("queue_failures", 1), ("event_backlog", 9)):
        changed = json.loads(json.dumps(final)); changed["stats"]["client_owner"][key] = value; invalid.append(changed)
    changed = json.loads(json.dumps(final)); changed["stats"]["ack_input"] = 10; invalid.append(changed)
    changed = json.loads(json.dumps(final)); changed["stats"]["raw_input"]["dropped"] = 1; invalid.append(changed)
    changed = json.loads(json.dumps(final)); changed["stats"]["raw_input"]["queued"] = 9; invalid.append(changed)
    changed = json.loads(json.dumps(final)); changed["stats"]["debt"] = 9; invalid.append(changed)
    for bad in invalid:
        try:
            stress_continuity(sample, bad, "mp_crash")
        except ValueError:
            pass
        else:
            raise AssertionError("Frozen admission, changed ownership or growing queues/debt accepted")
    archived = {"settings": {"cg_fovScale": "1", "com_maxfps": "0", "r_vsync": "0", "snd_khz": "44"}, "bindings": {"W": "+forward"}}
    assert preserved_preferences(archived, archived)["passed"]
    changed = json.loads(json.dumps(archived)); changed["settings"]["r_vsync"] = "1"
    assert not preserved_preferences(archived, changed)["passed"]
    changed = json.loads(json.dumps(archived)); changed["bindings"]["W"] = "+back"
    assert not preserved_preferences(archived, changed)["passed"]
    print("Options command timeout, real stress continuity and preference-preservation schemas passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-port", type=int, default=28992)
    parser.add_argument("--map", choices=("mp_killhouse", "mp_crash"), default="mp_crash")
    parser.add_argument("--prefix", action="store_true", help="First exercise the five categories preceding the original failure")
    parser.add_argument("--stress-rounds", type=int, default=0, help="Instead repeat all 12 real menu open/back actions, 1..5 rounds")
    parser.add_argument("--stress-seconds", type=int, default=10, help="Maximum stress action time, 2..15 seconds")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if os.name != "nt" or not 1024 <= args.client_port <= 65535:
        parser.error("The native Options fixture requires Windows and a port in 1024..65535")
    if not 0 <= args.stress_rounds <= 5 or not 2 <= args.stress_seconds <= 15 or (args.prefix and args.stress_rounds):
        parser.error("Choose prefix or stress mode; stress requires 1..5 rounds and 2..15 seconds")
    project = Path(__file__).resolve().parent.parent
    run_id = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    output = (args.output or project / "results" / ("native-options-" + run_id)).resolve()
    if output.exists() or not output.is_relative_to(project / "results") or output == project / "results":
        parser.error("Output must be a new dedicated project results directory")
    output.mkdir(parents=True)
    fixture = ObservedFixture(project, output, args.client_port, budget=45)
    fixture.report["options_probe_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    fixture.report["interactive_preferences_before"] = interactive_preferences(project)
    try:
        run(fixture, args.prefix, args.map, args.stress_rounds, args.stress_seconds)
    except Exception as error:
        fixture.report["errors"].append(str(error))
    finally:
        fixture.observe = False
        fixture.report["test_elapsed_seconds"] = time.monotonic() - fixture.started
        fixture.close()
        try:
            after = fixture.report["interactive_preferences_after"] = interactive_preferences(project)
            fixture.report["interactive_preferences_unchanged"] = after == fixture.report["interactive_preferences_before"]
            if not fixture.report["interactive_preferences_unchanged"]:
                fixture.report["errors"].append("Native benchmark Options changed interactive preferences")
                fixture.report["passed"] = False
        except (OSError, ValueError) as error:
            fixture.report["errors"].append("Preference verification: " + str(error))
            fixture.report["passed"] = False
        (output / "result.json").write_text(json.dumps(fixture.report, indent=2), encoding="utf8")
    print(json.dumps({"passed": fixture.report["passed"], "errors": fixture.report["errors"], "output": str(output / "result.json")}, indent=2))
    return 0 if fixture.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
