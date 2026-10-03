"""Short cap diagnostic on one exact owned native client/server pair.

Each cap has two seconds of warmup and eight seconds of samples. Run 480p and
720p as separate fresh fixtures. This is continuous native gameplay, not a
restored identical-input/full-world renderer replay or sustained timing gate.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import time

from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import interactive_preferences, wait_recovery
from render_cap_probe import CAPS, RATE_TOLERANCE_HZ, run_stage


class ManagedCapServer:
    """Use the fixture's single paced RCON clock and exact child identity."""
    def __init__(self, fixture):
        self.fixture = fixture

    def reset_work(self):
        self.fixture.rcon("khz_resetstats", fire=True)

    def sample(self):
        return self.fixture.native_server()


def admission_checks(stage, fixture, resolution):
    samples = stage["samples"]
    first, last = samples[0]["stats"], samples[-1]["stats"]
    duration = last["seconds"] - first["seconds"]
    ack_delta = last.get("ack_input", -1) - first.get("ack_input", -1)
    ack_hz = ack_delta / duration if duration > 0 else 0
    ack_pass = (type(first.get("ack_input")) is int and type(last.get("ack_input")) is int and
                ack_delta > 0 and abs(ack_hz - 1000) <= RATE_TOLERANCE_HZ)
    before, after = stage["first_server"], stage["last_server"]
    reject_delta = after.get("time_rejected", -1) - before.get("time_rejected", -1)
    rejects_pass = (type(before.get("time_rejected")) is int and type(after.get("time_rejected")) is int and reject_delta == 0)
    owner = first.get("client_owner", {})
    expected_dimensions = {"480p": (640, 480), "720p": (1280, 720)}[resolution]
    client_identity_pass = all(
        sample["stats"].get("process_id") == fixture.client.pid and
        sample["stats"].get("client_owner", {}).get("native_connection") == 9 and
        sample["stats"].get("client_owner", {}).get("native_cgame_initialized") is True and
        all(sample["stats"].get("client_owner", {}).get(name) == owner.get(name) and owner.get(name, 0) > 0
            for name in ("session", "generation")) and
        (sample["stats"].get("presentation_state", {}).get("width"),
         sample["stats"].get("presentation_state", {}).get("height")) == expected_dimensions
        for sample in samples)
    server_identity_pass = (fixture.server.alive() and before.get("process_id") == fixture.server.pid and
                            after.get("process_id") == fixture.server.pid)
    passed = ack_pass and rejects_pass and client_identity_pass and server_identity_pass
    return {"passed": passed, "accepted_ack_input_delta": ack_delta, "accepted_ack_input_hz": ack_hz,
            "accepted_ack_input_pass": ack_pass, "new_time_rejected": reject_delta,
            "new_time_rejected_pass": rejects_pass, "exact_client_identity_pass": client_identity_pass,
            "exact_server_identity_pass": server_identity_pass, "actual_resolution": list(expected_dimensions)}


def bot_control_rates(stage):
    before, after = stage["first_server"], stage["last_server"]
    duration = after["seconds"] - before["seconds"]
    first = before.get("native_bots", {}).get("slots", [])
    last = after.get("native_bots", {}).get("slots", [])
    result = {"passed": False, "slots": {}, "reason": "Missing, unstable or incomplete actual bot-control telemetry"}
    if duration <= 0 or len(first) != 15 or len(last) != 15:
        return result
    first_slots, last_slots = {slot.get("slot"): slot for slot in first}, {slot.get("slot"): slot for slot in last}
    if len(first_slots) != 15 or first_slots.keys() != last_slots.keys():
        return result
    for index, start in first_slots.items():
        finish = last_slots[index]
        valid = (type(start.get("generation")) is int and start["generation"] > 0 and finish.get("generation") == start["generation"] and
                 type(start.get("command_ticks")) is int and type(finish.get("command_ticks")) is int and
                 finish["command_ticks"] >= start["command_ticks"])
        hz = (finish["command_ticks"] - start["command_ticks"]) / duration if valid else None
        result["slots"][str(index)] = {"generation": start.get("generation"), "command_ticks_hz": hz,
                                      "passed": valid and abs(hz - 1000) <= RATE_TOLERANCE_HZ}
    result["passed"] = all(slot["passed"] for slot in result["slots"].values())
    if result["passed"]:
        result["reason"] = ""
    return result


def run(fixture, resolution, map_name):
    fixture.start()
    report = fixture.report
    report.update(resolution=resolution, map=map_name, caps=list(CAPS), stages=[], rate_tolerance_hz=RATE_TOLERANCE_HZ,
                  scope="Actual fixed-clock private pair with continuous native benchmark input; 2 s warmup and 8 s samples per cap. "
                        "Actual accepted acknowledgements and independent server admission are required. Uncapped FPS is a separate hardware goal. "
                        "This does not prove restored full-world replay or ten-minute stability.")
    if map_name != "mp_killhouse":
        before = fixture.sample()
        report["map_request"] = fixture.call("khz_ui server map " + map_name)
        if report["map_request"].get("result") != "server command queued":
            raise ValueError("Actual owned map request was rejected")
        wait_recovery(fixture, "render-cap target map", map_name, before, changed_map=True)
    args = SimpleNamespace(client_port=fixture.port, map=map_name, resolution=resolution, desired_bots=15)
    server = ManagedCapServer(fixture)
    log = fixture.server_home / "main/khz_server.log"
    initial_script_errors = log.read_text(encoding="latin1").count("******* script runtime error *******") if log.exists() else 0
    for cap in CAPS:
        stage = run_stage(cap, args, server)
        report["stages"].append(stage)
        stage["admission"] = admission_checks(stage, fixture, resolution)
        stage["bot_controls"] = bot_control_rates(stage)
        stage["passed"] = stage["passed"] and stage["admission"]["passed"] and stage["bot_controls"]["passed"]
        if not stage["passed"]:
            report["errors"].append(f"Cap {cap} failed one or more actual logic, admission, cap, collision, owner, work, debt or GPU checks")
        print(f"Cap {cap or 'uncapped'}: {stage['client']['gpu_presents_hz']:.1f} FPS; "
              f"accepted {stage['admission']['accepted_ack_input_hz']:.3f} Hz; passed={stage['passed']}", flush=True)
    uncapped = next(stage for stage in report["stages"] if stage["cap"] == 0)
    report["uncapped_fps_goal_pass"] = uncapped["exceeded_1000_fps"]
    final_script_errors = log.read_text(encoding="latin1").count("******* script runtime error *******") if log.exists() else 0
    report["new_script_errors"] = max(0, final_script_errors - initial_script_errors)
    if report["new_script_errors"]:
        report["errors"].append("Native cap fixture produced new script runtime errors")
    report["passed"] = len(report["stages"]) == len(CAPS) and all(stage["passed"] for stage in report["stages"]) and not report["errors"]
    fixture.leave()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolution", choices=("480p", "720p"), default="480p")
    parser.add_argument("--map", choices=("mp_killhouse", "mp_crash"), default="mp_killhouse")
    parser.add_argument("--client-port", type=int, default=28991)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--fullscreen", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        # Only schema/continuity policy; native cap and renderer behavior must
        # be established by the owned live fixture, never these mock values.
        class Alive:
            pid = 12
            @staticmethod
            def alive():
                return True
        fixture = SimpleNamespace(client=SimpleNamespace(pid=11), server=Alive())
        owner = {"native_connection": 9, "native_cgame_initialized": True, "session": 1, "generation": 2}
        first = {"seconds": 0, "ack_input": 1, "process_id": 11, "client_owner": owner,
                 "presentation_state": {"width": 640, "height": 480}}
        last = {**first, "seconds": 8, "ack_input": 8001}
        stage = {"samples": [{"stats": first}, {"stats": last}],
                 "first_server": {"process_id": 12, "time_rejected": 0}, "last_server": {"process_id": 12, "time_rejected": 0}}
        assert admission_checks(stage, fixture, "480p")["passed"]
        for change in ({"ack_input": 1}, {"process_id": 99}, {"client_owner": {**owner, "native_connection": 6}},
                       {"client_owner": {**owner, "generation": 3}}, {"presentation_state": {"width": 1280, "height": 720}}):
            invalid = {**stage, "samples": [{"stats": first}, {"stats": {**last, **change}}]}
            assert not admission_checks(invalid, fixture, "480p")["passed"]
        assert not admission_checks({**stage, "last_server": {"process_id": 12, "time_rejected": 1}}, fixture, "480p")["passed"]
        bots = [{"slot": i, "generation": i + 1, "command_ticks": 1} for i in range(15)]
        bot_stage = {"first_server": {"seconds": 0, "native_bots": {"slots": bots}},
                     "last_server": {"seconds": 8, "native_bots": {"slots": [dict(bot, command_ticks=8001) for bot in bots]}}}
        assert bot_control_rates(bot_stage)["passed"]
        assert not bot_control_rates({**bot_stage, "last_server": {"seconds": 8, "native_bots": {"slots": bots}}})["passed"]
        assert not bot_control_rates({**bot_stage, "last_server": {"seconds": 8, "native_bots": {"slots": bots[:-1]}}})["passed"]
        print("Owned cap-fixture admission/lifetime/resolution schemas passed; no native rendering behavior claimed")
        return 0
    project = Path(__file__).resolve().parent.parent
    run_id = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    output = (args.output or project / "results" / ("native-render-caps-" + args.resolution + "-" + run_id)).resolve()
    if not 1024 <= args.client_port <= 65535 or output.exists() or not output.is_relative_to(project / "results") or output == project / "results":
        parser.error("Use a valid port and a fresh confined result directory")
    output.mkdir(parents=True)
    fixture = ManagedHostFixture(project, output, args.client_port, resolution=args.resolution, fullscreen=args.fullscreen)
    fixture.report["render_cap_probe_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    prefs = None
    try:
        prefs = fixture.report["interactive_preferences_before"] = interactive_preferences(project)
        run(fixture, args.resolution, args.map)
    except Exception as error:
        fixture.report["errors"].append(str(error))
        fixture.report["passed"] = False
    finally:
        fixture.report["test_elapsed_seconds"] = time.monotonic() - fixture.started
        fixture.close()
        try:
            fixture.report["interactive_preferences_after"] = interactive_preferences(project)
            fixture.report["interactive_preferences_unchanged"] = prefs is not None and prefs == fixture.report["interactive_preferences_after"]
            fixture.report["binary_sha256_after_cleanup"] = {
                "client": hashlib.sha256(fixture.binary.read_bytes()).hexdigest(),
                "server": hashlib.sha256(fixture.server_binary.read_bytes()).hexdigest()}
            fixture.report["binary_provenance_pass"] = fixture.report["binary_sha256"] == fixture.report["binary_sha256_after_cleanup"]
            if not fixture.report["interactive_preferences_unchanged"] or not fixture.report["binary_provenance_pass"]:
                raise ValueError("Fixture preferences or binaries changed across the exact owned lifetime")
        except (OSError, ValueError) as error:
            fixture.report["errors"].append("Post-cleanup cap provenance: " + str(error))
            fixture.report["passed"] = False
        (output / "result.json").write_text(json.dumps(fixture.report, indent=2), encoding="utf8")
    print(json.dumps({"passed": fixture.report["passed"], "errors": fixture.report["errors"], "output": str(output / "result.json")}, indent=2))
    return 0 if fixture.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
