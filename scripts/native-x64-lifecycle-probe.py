"""Exercise the packaged AMD64 client and its owned server through real restarts."""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time

from monitor import query
from native_lifecycle_bounded_probe import OwnedServer, continuity, native_map_matches

root = Path(__file__).resolve().parent.parent
client = root / "runtime/x64/cod4x1khz.exe"
server = root / "runtime/x64/cod4x1khz_server.exe"
probe = root / "build/x64/Release/Release/khz_launch_probe.exe"
prefix = root / "results/x64-native-packaged-lifecycle"
port = 28989
report = {"binaries": {}, "stages": [], "cleanup": {}}
for binary in (client, server):
    data = binary.read_bytes()
    offset = struct.unpack_from("<I", data, 0x3c)[0]
    if data[offset:offset + 4] != b"PE\0\0" or struct.unpack_from("<H", data, offset + 4)[0] != 0x8664:
        raise ValueError(f"Expected a native AMD64 executable: {binary}")
    report["binaries"][str(binary.relative_to(root))] = hashlib.sha256(data).hexdigest()
# Refuse to use a diagnostic endpoint that belongs to an existing session.
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as endpoint:
    endpoint.bind(("127.0.0.1", port))

env = dict(os.environ, KHZ_PROBE_SECONDS="360", KHZ_PROBE_HIDE_WINDOW="1")
env.pop("KHZ_WATCH_HUNK", None)
env.pop("KHZ_WATCH_SYMBOL", None)
arguments = (f"+set net_port {port} +set khz_benchmark 1 +set khz_test_controls 1 "
             "+set com_introPlayed 1 +set r_customMode 1280x720 +set r_graphicsApi d3d9 "
             "+set r_vsync 0 +set com_maxfps 1000 +set logfile 2 +set cl_timeout 300 +set cl_connectTimeout 300")
owned_server = None
client_pid = None
started = time.monotonic()

def call(command, marker="KHZ_UI"):
    if owned.poll() is not None:
        raise RuntimeError(f"Created client exited: {owned.returncode}")
    return query(port, command, marker, 1)

def sample():
    result = {"elapsed": time.monotonic() - started,
              "stats": call("khz_stats", "KHZ_JSON"),
              "state": call("khz_state", "KHZ_STATE"),
              "ui": call("khz_ui status")}
    if client_pid is not None and result["stats"].get("process_id") != client_pid:
        raise ValueError("Diagnostic endpoint changed process")
    return result

def wait_ready(expected_map, timeout=45, previous=None, changed=None):
    deadline = time.monotonic() + timeout
    attempts = []
    threads_captured = False
    while time.monotonic() < deadline:
        try:
            current = sample()
            host = current["ui"].get("host", {})
            owner = current["stats"].get("client_owner", {})
            ready = (host.get("owned") is True and host.get("authenticated") is True
                     and host.get("phase") == "ready" and host.get("map") == expected_map
                     and native_map_matches(current["state"].get("map"), expected_map)
                     and current["state"].get("connection") == 9
                     and owner.get("active") is True and owner.get("phase") == "running"
                     and current["stats"].get("prediction_ticks", 0) > 0
                     and not current["stats"].get("presentation_state", {}).get("engine_error", True))
            if previous is not None:
                before = previous["stats"]["client_owner"]
                if changed == "generation":
                    ready = ready and owner.get("generation", 0) > before.get("generation", 0)
                elif changed == "session":
                    ready = ready and owner.get("session") != before.get("session")
            if ready:
                time.sleep(2)
                after = sample()
                result = {"map": expected_map, "first": current, "last": after, "attempts": attempts}
                try:
                    result["continuity"] = continuity(current, after)
                except ValueError as error:
                    attempts.append({"error": str(error), "first_presents": current["stats"]["gpu_presents"],
                                     "last_presents": after["stats"]["gpu_presents"], "elapsed": after["elapsed"]})
                    continue
                report["stages"].append(result)
                print(json.dumps({"map": expected_map, "continuity": result["continuity"]}), flush=True)
                return after
        except (TimeoutError, OSError) as error:
            attempts.append(str(error))
            if len(attempts) >= 4 and not threads_captured and client_pid is not None:
                threads_captured = True
                with prefix.with_suffix(".threads.log").open("w") as threads:
                    subprocess.run([str(probe.parent / "khz_thread_probe.exe"), str(client_pid), str(client),
                                    str(root / "bin/x64/Release")], stdout=threads,
                                   stderr=subprocess.STDOUT, timeout=25)
        time.sleep(.5)
    raise TimeoutError(f"Native lifecycle did not reach active {expected_map}")

with prefix.with_suffix(".log").open("w") as logfile:
    owned = subprocess.Popen([str(probe), str(client), str(root), str(client.parent), arguments],
                             cwd=root, env=env, stdout=logfile, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                initial = sample()
                client_pid = initial["stats"]["process_id"]
                if initial["ui"].get("top_menu") in ("main", "main_text"):
                    break
            except (TimeoutError, OSError):
                pass
            time.sleep(.5)
        else:
            raise TimeoutError("Packaged native client did not open the main menu")
        report["start"] = call("khz_ui host mp_backlot dm 2 1")
        previous = wait_ready("mp_backlot")
        child_pid = previous["ui"]["host"]["child_pid"]
        owned_server = OwnedServer(child_pid, server)
        report["child_pid"] = child_pid
        for name, command, expected_map, changed in (
                ("map", "server map mp_crash", "mp_crash", "session"),
                ("fast_restart", "server fast_restart", "mp_crash", "session"),
                ("vid_restart", "vid_restart", "mp_crash", "generation"),
                ("snd_restart", "snd_restart", "mp_crash", "generation")):
            report["request"] = {"stage": name, "reply": call("khz_ui " + command)}
            previous = wait_ready(expected_map, timeout=90 if changed == "generation" else 45, previous=previous, changed=changed)
            report["stages"][-1]["stage"] = name
            if previous["ui"]["host"]["child_pid"] != child_pid or not owned_server.alive():
                raise ValueError("Owned native server changed or exited")
        report["leave"] = call("khz_ui leave")
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            try:
                left = sample()
                host = left["ui"].get("host", {})
                if (host.get("owned") is False and host.get("child_pid") == 0
                        and left["state"].get("connection") == 0
                        and left["ui"].get("top_menu") in ("main", "main_text")
                        and not owned_server.alive()):
                    report["left"] = left
                    break
            except (TimeoutError, OSError):
                pass
            time.sleep(.5)
        else:
            raise TimeoutError("Leave did not restore the menu and shut down the owned server")
        report["quit"] = call("khz_ui quit")
        report["client_exit"] = owned.wait(timeout=20)
        if report["client_exit"] != 0:
            raise ValueError("Packaged client did not exit cleanly")
        report["passed"] = True
    except Exception as error:
        report["passed"] = False
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if owned.poll() is None:
            try:
                report["cleanup"]["quit"] = call("khz_ui quit")
            except Exception as error:
                report["cleanup"]["quit_error"] = str(error)
            try:
                owned.wait(timeout=15)
            except subprocess.TimeoutExpired:
                owned.terminate()
                owned.wait(timeout=10)
        report["client_exit"] = owned.returncode
        if owned_server is not None:
            report["cleanup"]["server_alive"] = owned_server.alive()
            if owned_server.alive():
                owned_server.stop()
            owned_server.close()
        report["elapsed"] = time.monotonic() - started
        prefix.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({key: report.get(key) for key in ("passed", "error", "elapsed", "client_exit", "child_pid", "cleanup")}))
raise SystemExit(0 if report.get("passed") else 1)
