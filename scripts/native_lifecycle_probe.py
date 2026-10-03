"""Short native owned-host map/graphics/audio lifecycle probe; no computer use."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time

from monitor import query


def counter_delta(first, last, name):
    a, b = first.get(name), last.get(name)
    if type(a) is not int or type(b) is not int or a < 0 or b < a:
        raise ValueError(f"Missing, invalid or reset native counter: {name}")
    return b - a


def native_map_matches(value, expected):
    # Native CL.mapname is a BSP qpath, whereas the stock host uses a map id.
    # Accept only the exact known forms, rather than an arbitrary suffix match.
    return type(value) is str and value in (expected, f"maps/mp/{expected}.d3dbsp")


def ready_errors(sample, expected_map, client_pid, child_pid=None):
    """Read explicit native ownership metadata; active is not a connection proxy."""
    stats, state, ui = (sample.get(key, {}) for key in ("stats", "state", "ui"))
    owner, screen, host = (stats.get("client_owner", {}), stats.get("presentation_state", {}), ui.get("host", {}))
    errors = []
    if stats.get("process_id") != client_pid:
        errors.append("diagnostic endpoint process differs from the created client")
    for name, value in (("main native connection", screen.get("connection")),
                        ("client state connection", state.get("connection")),
                        ("owner native connection", owner.get("native_connection"))):
        if type(value) is not int or value != 9:
            errors.append(f"{name} is not explicitly CA_ACTIVE (9)")
    if not native_map_matches(state.get("map"), expected_map):
        errors.append("native client map differs")
    if owner.get("active") is not True or owner.get("private_domains") is not True or owner.get("phase") != "running":
        errors.append("private gameplay owner is not running")
    if owner.get("native_cgame_initialized") is not True:
        errors.append("native owner cgame is not explicitly initialized")
    for name in ("session", "generation", "ticks", "snapshots", "pose_ticks"):
        if type(owner.get(name)) is not int or owner[name] <= 0:
            errors.append(f"missing or inactive owner {name}")
    for name in ("commands_sent", "snapshots_received", "prediction_ticks", "gpu_presents"):
        if type(stats.get(name)) is not int or stats[name] <= 0:
            errors.append(f"missing or inactive client {name}")
    if screen.get("renderer_started") is not True or screen.get("screen_initialized") is not True:
        errors.append("native renderer/screen is not initialized")
    if screen.get("window_minimized") is not False or screen.get("engine_error") is not False:
        errors.append("native presentation is minimized or in engine error")
    if host.get("owned") is not True or host.get("authenticated") is not True or host.get("managed_server") is not False or host.get("phase") != "ready":
        errors.append("authenticated owned host is not ready")
    if type(host.get("child_pid")) is not int or host["child_pid"] <= 0 or (child_pid and host["child_pid"] != child_pid):
        errors.append("owned server PID differs or is absent")
    if host.get("map") != expected_map or host.get("gametype") != "dm":
        errors.append("owned native host map/rules differ")
    if host.get("bot_count") != 15 or host.get("bot_difficulty") != 1:
        errors.append("owned host does not retain 15 Regular bots")
    if host.get("failures") != 0 or owner.get("queue_failures") != 0:
        errors.append("native host or owner reported failures")
    return errors


def continuity(first, last):
    a, b = first["stats"], last["stats"]
    ao, bo = a["client_owner"], b["client_owner"]
    if any(ao.get(key) != bo.get(key) for key in ("session", "generation")):
        raise ValueError("Owner lifetime changed during the live continuity sample")
    client = {key: counter_delta(a, b, key) for key in ("commands_sent", "snapshots_received", "prediction_ticks", "gpu_presents", "ack_input")}
    owner = {key: counter_delta(ao, bo, key) for key in ("ticks", "snapshots", "pose_ticks")}
    if not all(value > 0 for value in (*client.values(), *owner.values())):
        raise ValueError("Native network, authoritative input acknowledgement, gameplay, accepted snapshots, pose or GPU presentation stopped")
    return {"seconds": last["elapsed"] - first["elapsed"], "client_counter_deltas": client,
            "owner_counter_deltas": owner, "session": bo["session"], "generation": bo["generation"]}


def server_continuity(first, last, client_port):
    """Transport attempts alone do not establish authoritative admission."""
    counters = {name: counter_delta(first, last, name) for name in ("commands_received", "snapshots_sent", "time_rejected")}
    first_peers = [peer for peer in first.get("peers", []) if peer.get("port") == client_port]
    last_peers = [peer for peer in last.get("peers", []) if peer.get("port") == client_port]
    if len(first_peers) != 1 or len(last_peers) != 1 or first_peers[0].get("client") != last_peers[0].get("client"):
        raise ValueError("Actual native server human peer changed or is missing")
    peer = {name: counter_delta(first_peers[0], last_peers[0], name) for name in ("commands", "snapshots")}
    if not all(value > 0 for value in (counters["commands_received"], counters["snapshots_sent"], *peer.values())):
        raise ValueError("Actual server admission, unique human commands or authoritative snapshots stopped")
    if counters["time_rejected"]:
        raise ValueError(f"Actual server rejected {counters['time_rejected']} input timestamps during the fresh recovery samples")
    return {"server_counter_deltas": counters, "human_peer_counter_deltas": peer}


def transition_applied(stage, before, after):
    if stage == "host":
        return True
    previous, current = before["stats"]["client_owner"], after["stats"].get("client_owner", {})
    if stage == "map":
        return native_map_matches(after["state"].get("map"), "mp_crash") and (current.get("session") != previous.get("session") or after["state"].get("server_id") != before["state"].get("server_id"))
    if stage in ("fast_restart", "map_restart"):
        return type(current.get("session")) is int and current["session"] > 0 and current["session"] != previous.get("session")
    if stage in ("vid_restart", "snd_restart"):
        return type(current.get("generation")) is int and current["generation"] > previous.get("generation", 0)
    raise ValueError("Unknown lifecycle stage")


class OwnedServer:
    """Retain an exact process handle, rather than later trusting a reused PID."""
    def __init__(self, pid, executable):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.TerminateProcess.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        self.kernel, self.pid, self.handle = kernel, pid, None
        handle = kernel.OpenProcess(0x1000 | 0x100000 | 0x1, False, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        path, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(length)):
            kernel.CloseHandle(handle)
            raise ctypes.WinError(ctypes.get_last_error())
        if Path(path.value).resolve() != executable.resolve():
            kernel.CloseHandle(handle)
            raise RuntimeError("Owned-host PID is not the exact project server executable")
        self.handle, self.path = handle, path.value

    def alive(self):
        result = self.kernel.WaitForSingleObject(self.handle, 0)
        if result not in (0, 258):
            raise ctypes.WinError(ctypes.get_last_error())
        return result == 258

    def stop(self):
        if self.alive() and not self.kernel.TerminateProcess(self.handle, 1):
            raise ctypes.WinError(ctypes.get_last_error())
        if self.kernel.WaitForSingleObject(self.handle, 5000) != 0:
            raise RuntimeError("Exact owned server handle did not terminate")

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def windows_for_pid(pid):
    """Metadata only: no focusing, key injection or desktop screenshots."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.EnumWindows.restype = wintypes.BOOL
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    user.IsWindowVisible.argtypes = user.IsIconic.argtypes = [wintypes.HWND]
    user.IsWindowVisible.restype = user.IsIconic.restype = wintypes.BOOL
    user.GetWindowTextW.argtypes = user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.GetClientRect.restype = wintypes.BOOL
    result = []

    @callback_type
    def collect(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            title, classname, rect = ctypes.create_unicode_buffer(256), ctypes.create_unicode_buffer(128), wintypes.RECT()
            user.GetWindowTextW(hwnd, title, len(title)); user.GetClassNameW(hwnd, classname, len(classname))
            rectangle = [rect.left, rect.top, rect.right, rect.bottom] if user.GetClientRect(hwnd, ctypes.byref(rect)) else None
            result.append({"hwnd": int(hwnd), "visible": bool(user.IsWindowVisible(hwnd)), "minimized": bool(user.IsIconic(hwnd)),
                           "title": title.value, "class": classname.value, "client_rect": rectangle})
        return True

    if not user.EnumWindows(collect, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    return result


def self_test():
    owner = {"active": True, "private_domains": True, "phase": "running", "native_connection": 9,
             "native_cgame_initialized": True, "session": 11, "generation": 3, "ticks": 10, "snapshots": 10, "pose_ticks": 10, "queue_failures": 0}
    host = {"owned": True, "authenticated": True, "managed_server": False, "phase": "ready", "child_pid": 22,
            "map": "mp_killhouse", "gametype": "dm", "bot_count": 15, "bot_difficulty": 1, "failures": 0}
    stats = {"process_id": 21, "client_owner": owner, "commands_sent": 10, "snapshots_received": 10, "prediction_ticks": 10, "gpu_presents": 10, "ack_input": 10,
             "presentation_state": {"connection": 9, "renderer_started": True, "screen_initialized": True, "window_minimized": False, "engine_error": False}}
    first = {"stats": stats, "state": {"map": "mp_killhouse", "connection": 9, "server_id": 1}, "ui": {"host": host}, "elapsed": 1}
    assert not ready_errors(first, "mp_killhouse", 21, 22)
    bsp = {**first, "state": {**first["state"], "map": "maps/mp/mp_killhouse.d3dbsp"}}
    assert not ready_errors(bsp, "mp_killhouse", 21, 22)
    for value in (None, "other/maps/mp/mp_killhouse.d3dbsp", "maps/mp/mp_crash.d3dbsp", "../mp_killhouse"):
        assert not native_map_matches(value, "mp_killhouse")
    for field, value in (("native_connection", None), ("native_connection", 6), ("native_cgame_initialized", None), ("snapshots", 0), ("pose_ticks", True)):
        invalid = {**first, "stats": {**stats, "client_owner": {**owner, field: value}}}
        assert ready_errors(invalid, "mp_killhouse", 21, 22)
    assert ready_errors(first, "mp_crash", 21, 22) and ready_errors(first, "mp_killhouse", 21, 23)
    after = json.loads(json.dumps(first)); after["elapsed"] = 2
    for field in ("commands_sent", "snapshots_received", "prediction_ticks", "gpu_presents", "ack_input"):
        after["stats"][field] += 10
    for field in ("ticks", "snapshots", "pose_ticks"):
        after["stats"]["client_owner"][field] += 10
    assert continuity(first, after)["client_counter_deltas"]["gpu_presents"] == 10
    invalid = json.loads(json.dumps(after)); invalid["stats"]["gpu_presents"] = 10
    try:
        continuity(first, invalid)
    except ValueError:
        pass
    else:
        raise AssertionError("Stopped GPU presentation passed")
    invalid = json.loads(json.dumps(after)); invalid["stats"]["ack_input"] = 10
    try:
        continuity(first, invalid)
    except ValueError:
        pass
    else:
        raise AssertionError("Frozen authoritative input acknowledgement passed advancing transport attempts")
    native_before = {"commands_received": 10, "snapshots_sent": 10, "time_rejected": 3,
                     "peers": [{"client": 0, "port": 28993, "commands": 10, "snapshots": 10}]}
    native_after = {"commands_received": 20, "snapshots_sent": 20, "time_rejected": 3,
                    "peers": [{"client": 0, "port": 28993, "commands": 20, "snapshots": 20}]}
    assert server_continuity(native_before, native_after, 28993)["server_counter_deltas"]["time_rejected"] == 0
    for invalid in ({**native_after, "time_rejected": 4}, {**native_after, "time_rejected": None},
                    {**native_after, "commands_received": 10}, {**native_after, "peers": []}):
        try:
            server_continuity(native_before, invalid, 28993)
        except ValueError:
            pass
        else:
            raise AssertionError("Stopped/rejected or missing actual server admission passed")
    for stage in ("map", "fast_restart", "map_restart", "vid_restart", "snd_restart"):
        assert not transition_applied(stage, first, after)
    after["stats"]["client_owner"].update(session=12, generation=4)
    after["state"].update(map="mp_crash", server_id=2)
    for stage in ("map", "fast_restart", "map_restart", "vid_restart", "snd_restart"):
        assert transition_applied(stage, first, after)
    print("Native lifecycle schema, transition and stopped-counter fixtures passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-port", type=int, default=28993)
    parser.add_argument("--timeout", type=float, default=30, help="Maximum readiness time per stage")
    parser.add_argument("--total-timeout", type=float, default=90, help="Overall native test budget, excluding owned cleanup")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(); return 0
    if os.name != "nt":
        parser.error("The actual native lifecycle probe requires Windows")
    if not 1024 <= args.client_port <= 65535 or not 5 <= args.timeout <= 30 or not 15 <= args.total_timeout <= 90:
        parser.error("Port must be 1024..65535, per-stage timeout 5..30s, and overall budget 15..90s")
    project = Path(__file__).resolve().parent.parent
    run = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f") + f"-{os.getpid()}"
    output = (args.output or project / "results" / ("native-lifecycle-" + run)).resolve()
    if output.exists() or output == project / "results" or not output.is_relative_to(project / "results"):
        parser.error("Output must be a new dedicated project results directory")
    binary, server_binary = project / "cod4x1khz.exe", project / "runtime/cod4x1khz_server.exe"
    if not binary.is_file() or not server_binary.is_file():
        parser.error("Build the root client and runtime server first")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", args.client_port))
    home = (project / "runtime/benchmarks" / f"client-{args.client_port}").resolve()
    if home.parent != (project / "runtime/benchmarks").resolve() or home.is_symlink():
        parser.error("Benchmark home must be a direct non-symlink project child")
    output.mkdir(parents=True)
    report = {"passed": False, "errors": [], "stages": [], "client_port": args.client_port, "fresh_benchmark_home": str(home),
              "started_utc": datetime.now(timezone.utc).isoformat(), "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "binary_sha256": {
                  "client": hashlib.sha256(binary.read_bytes()).hexdigest(), "server": hashlib.sha256(server_binary.read_bytes()).hexdigest()},
              "scope": "Actual root executable launch from C:\\Windows\\Temp, isolated fresh benchmark profile, stock main menu, "
                       "native owned Killhouse DM with 15 Regular bots, Crash/map/graphics/audio recovery, live native client/owner/network/GPU counters, "
                       "and explicit owned-server Leave. Short diagnostic only; no 1 kHz timing, uncapped FPS, full-world replay or complete public parity acceptance claim."}
    saved_home = output / "previous-client-home"
    client, server, server_home, started = None, None, None, time.monotonic()
    fixture_home_authorized = False
    deadline = started + args.total_timeout
    server_last_query = float("-inf")

    def client_live():
        if client.poll() is not None:
            raise RuntimeError(f"Created client {client.pid} exited with {client.returncode}")

    def call(command, marker="KHZ_UI", until=None):
        client_live()
        remaining = (deadline if until is None else min(deadline, until)) - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Native lifecycle overall/stage readiness budget exhausted")
        return query(args.client_port, command, marker, min(1.2, remaining))

    def sample(until):
        stats = call("khz_stats", "KHZ_JSON", until)
        if stats.get("process_id") != client.pid:
            raise RuntimeError("Client diagnostic endpoint is not the created process")
        return {"elapsed": time.monotonic() - started, "stats": stats,
                "state": call("khz_state", "KHZ_STATE", until), "ui": call("khz_ui status", until=until)}

    def recovery_server_sample(entry, until):
        nonlocal server_last_query
        pause = server_last_query + 1.1 - time.monotonic()
        if pause > 0:
            time.sleep(min(pause, max(0, until - time.monotonic())))
        remaining = min(deadline, until) - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Native server recovery sample exhausted the bounded stage budget")
        if not server or not server.alive():
            raise RuntimeError("Exact owned server exited before a fresh recovery sample")
        server_last_query = time.monotonic()
        value = query(report["owned_server"]["port"], "rcon local-khz-benchmark khz_json", "KHZ_JSON", min(1.2, remaining))
        if value.get("process_id") != server.pid:
            raise RuntimeError("Owned server diagnostic endpoint differs from the exact child")
        # Store every independent sample, including failed readiness attempts.
        entry.setdefault("server_samples", []).append({"elapsed": time.monotonic() - started, "stats": value})
        entry["server"] = value
        peers = [peer for peer in value.get("peers", []) if peer.get("port") == args.client_port]
        bots = value.get("native_bots", {})
        if len(peers) != 1 or peers[0].get("commands", 0) <= 0 or peers[0].get("snapshots", 0) <= 0 or bots.get("active") != 15 or bots.get("humans") != 1 or bots.get("difficulty") != 1:
            raise ValueError("Actual native server does not yet report the human, commands/snapshots and 15 bots")
        counter_delta(value, value, "time_rejected")
        return value

    def stalled_server_sample(entry, label, status=False):
        """Independent server evidence even when the client stops servicing OOB."""
        nonlocal server_last_query
        evidence = {"label": label, "command": "status" if status else "khz_json"}
        entry.setdefault("independent_server_samples", []).append(evidence)
        try:
            if not server or not server.alive():
                raise RuntimeError("Exact owned server is unavailable for failure evidence")
            pause = server_last_query + 1.1 - time.monotonic()
            if pause > 0:
                time.sleep(pause)
            server_last_query = time.monotonic()
            evidence["elapsed"] = server_last_query - started
            port = report["owned_server"]["port"]
            if not status:
                value = query(port, "rcon local-khz-benchmark khz_json", "KHZ_JSON", 1.2)
                if value.get("process_id") != server.pid:
                    raise RuntimeError("Failure evidence endpoint differs from the exact owned server")
                evidence["stats"] = value
            else:
                # Stock status is text. Preserve its complete bounded receive
                # window without guessing a JSON marker or a client response.
                fragments = bytearray()
                until = time.monotonic() + 1.2
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.sendto(b"\xff" * 4 + b"rcon local-khz-benchmark status", ("127.0.0.1", port))
                    while time.monotonic() < until:
                        sock.settimeout(until - time.monotonic())
                        try:
                            data, source = sock.recvfrom(65536)
                        except socket.timeout:
                            break
                        if source != ("127.0.0.1", port):
                            continue
                        if data.startswith(b"\xff" * 4):
                            data = data[4:]
                        if data.startswith(b"print\n"):
                            data = data[6:]
                        if len(fragments) + len(data) > 65536:
                            raise ValueError("Owned server status exceeds the bounded diagnostic buffer")
                        fragments.extend(data)
                if not fragments:
                    raise TimeoutError("Owned server status returned no text")
                evidence["text"] = fragments.decode("latin1")
            evidence["finished_elapsed"] = time.monotonic() - started
        except Exception as error:
            evidence["error"] = str(error)

    def inspect_stalled_client(entry):
        inspector = project / "build/Release/Release/khz_inspect.exe"
        symbols = project / "engine/bin/Release"
        evidence = {"timeout_seconds": 10, "inspector": str(inspector), "symbols": str(symbols), "captures": []}
        entry["native_stack_capture"] = evidence
        if not inspector.is_file() or not symbols.is_dir():
            evidence["error"] = "Matching project native inspector/symbols are unavailable"
            return
        handle = None
        try:
            client_live()
            handle = OwnedServer(client.pid, binary)
            evidence["inspector_sha256"] = hashlib.sha256(inspector.read_bytes()).hexdigest()
            stalled_server_sample(entry, "before_native_dumps")
            for index in range(2):
                if index:
                    # Separate complete samples; the target runs normally in
                    # between, so lock ownership and CPU progress are useful.
                    time.sleep(2)
                if not handle.alive():
                    raise RuntimeError("Created client exited before native inspection")
                destination = output / (entry["name"] + "-native-stacks" + ("-2" if index else "") + ".txt")
                capture = {"started_elapsed": time.monotonic() - started, "started_utc": datetime.now(timezone.utc).isoformat(), "path": str(destination)}
                evidence["captures"].append(capture)
                try:
                    # The tool briefly suspends/resumes only this verified game.
                    result = subprocess.run([str(inspector), str(client.pid), str(symbols)], cwd=project,
                                            capture_output=True, text=True, errors="replace", timeout=10)
                    capture["returncode"] = result.returncode
                    destination.write_text(result.stdout + result.stderr, encoding="utf8")
                except subprocess.TimeoutExpired as error:
                    capture["error"] = "Native inspector exceeded its 10-second limit"
                    pieces = [part.decode("utf8", errors="replace") if isinstance(part, bytes) else part or "" for part in (error.stdout, error.stderr)]
                    destination.write_text("".join(pieces), encoding="utf8")
                capture["finished_elapsed"] = time.monotonic() - started
                if not index:
                    # Retain the original report fields for earlier readers.
                    evidence.update({name: capture[name] for name in ("path", "returncode", "error") if name in capture})
                stalled_server_sample(entry, f"after_native_dump_{index + 1}")
            stalled_server_sample(entry, "after_native_dumps_status", status=True)
        except Exception as error:
            evidence["error"] = str(error)
        finally:
            if handle:
                handle.close()

    def wait_stage(stage, expected_map, previous=None):
        nonlocal server, server_home, server_last_query
        entry = {"name": stage, "map": expected_map, "passed": False, "samples": []}
        report["stages"].append(entry)
        until = min(deadline, time.monotonic() + args.timeout)
        first, first_server, consecutive_timeouts = None, None, 0
        while time.monotonic() < until:
            if server and not server.alive():
                raise RuntimeError("Exact owned server process exited during native lifecycle")
            try:
                current = sample(until)
                consecutive_timeouts = 0
                entry["samples"].append(current)
                host = current["ui"].get("host", {})
                if server is None and host.get("owned") is True and host.get("authenticated") is True and type(host.get("child_pid")) is int and host["child_pid"] > 0:
                    server = OwnedServer(host["child_pid"], server_binary)
                    server_home = project / "runtime/benchmarks" / f"server-{host['port']}"
                    report["owned_server"] = {"pid": server.pid, "path": server.path, "port": host["port"], "home": str(server_home)}
                errors = ready_errors(current, expected_map, client.pid, server.pid if server else None)
                entry["last_readiness_errors"] = errors
                applied = transition_applied(stage, previous, current) if previous else True
                if not errors and applied:
                    if first and current["elapsed"] - first["elapsed"] >= .25:
                        try:
                            entry["continuity"] = continuity(first, current)
                        except ValueError as error:
                            entry["last_continuity_error"] = str(error); first = first_server = None
                        else:
                            entry["windows"] = windows_for_pid(client.pid)
                            if not any(window["visible"] and not window["minimized"] for window in entry["windows"]):
                                raise RuntimeError("Created client has no visible, non-minimized native window")
                            # Two paced independent samples prove admission
                            # actually advances with no new time rejection.
                            try:
                                latest_server = recovery_server_sample(entry, until)
                                entry["server_continuity"] = server_continuity(first_server, latest_server, args.client_port)
                            except ValueError as error:
                                entry["last_readiness_errors"] = [str(error)]
                                first = first_server = None
                            else:
                                entry.update(passed=True, elapsed=time.monotonic() - started, recovered=current)
                                return current
                    elif first is None:
                        try:
                            first_server = recovery_server_sample(entry, until)
                        except ValueError as error:
                            entry["last_readiness_errors"] = [str(error)]
                            first = first_server = None
                        else:
                            first = current
                else:
                    first = first_server = None
            except (TimeoutError, OSError) as error:
                entry["last_query_error"] = str(error)
                first = first_server = None
                consecutive_timeouts = consecutive_timeouts + 1 if isinstance(error, TimeoutError) else 0
                if stage in ("vid_restart", "snd_restart") and consecutive_timeouts >= 2 and "native_stack_capture" not in entry:
                    inspect_stalled_client(entry)
            time.sleep(min(.12, max(0, until - time.monotonic())))
        raise RuntimeError(f"Native {stage} did not recover before its bounded deadline: {entry.get('last_readiness_errors', [])}; {entry.get('last_query_error', '')}")

    try:
        if home.exists():
            home.rename(saved_home)
            report["previous_home_preserved"] = True
        fixture_home_authorized = True
        arguments = [str(binary), "allowdupe", "+set", "net_port", str(args.client_port), "+set", "khz_benchmark", "1",
                     "+set", "khz_test_controls", "1", "+set", "com_introPlayed", "1", "+set", "r_mode", "640x480",
                     "+set", "r_customMode", "640x480", "+set", "r_fullscreen", "0"]
        cfg = next((path for path in (project / "runtime/main/client.cfg", project / "runtime/client.cfg") if path.is_file()), None)
        if cfg:
            arguments += ["+exec", "client.cfg"]
            report["benchmark_cfg"] = {"path": str(cfg), "sha256": hashlib.sha256(cfg.read_bytes()).hexdigest()}
        arguments += ["+set", "com_maxfps", "0", "+set", "r_vsync", "0", "+setqport", str(args.client_port)]
        report["launch"] = {"arguments": arguments, "cwd": r"C:\Windows\Temp", "auto_connect": False}
        client = subprocess.Popen(subprocess.list2cmdline(arguments), cwd=r"C:\Windows\Temp")
        report["owned_client_pid"] = client.pid
        until = min(deadline, time.monotonic() + args.timeout)
        startup = []
        report["startup_samples"] = startup
        while time.monotonic() < until:
            try:
                current = sample(until); startup.append(current)
                if current["ui"].get("top_menu") in ("main", "main_text") and current["state"].get("connection") == 0:
                    report["initial"] = current
                    ui = current["ui"]
                    if ui.get("fullscreen") is not False or ui.get("vsync") is not False or ui.get("render_cap") != 0 or ui.get("fov_scale") != 1:
                        raise RuntimeError("Fresh benchmark menu does not have expected windowed, VSync-off, uncapped, FOV-scale-one settings")
                    presentation = current["stats"].get("presentation_state", {})
                    if presentation.get("width") != 640 or presentation.get("height") != 480:
                        raise RuntimeError("Fresh benchmark renderer did not use the requested 640x480 fixture resolution")
                    if current["stats"].get("host", {}).get("owned") is not False:
                        raise RuntimeError("No-auto-connect startup unexpectedly owns a server")
                    report["initial_windows"] = windows_for_pid(client.pid)
                    break
            except (OSError, TimeoutError) as error:
                report["last_startup_query_error"] = str(error)
            time.sleep(.1)
        if "initial" not in report:
            raise RuntimeError("Root executable did not reach the stock disconnected main menu")
        launched = call("khz_ui host mp_killhouse dm 15 1")
        report["host_request"] = launched
        if launched.get("result") != "start requested":
            raise RuntimeError("Native stock StartServer did not accept the host request")
        current = wait_stage("host", "mp_killhouse")
        for stage, command in (("map", "server map mp_crash"), ("fast_restart", "server fast_restart"),
                               ("map_restart", "server map_restart"), ("vid_restart", "vid_restart"), ("snd_restart", "snd_restart")):
            reply = call("khz_ui " + command)
            report.setdefault("actions", []).append({"stage": stage, "command": command, "reply": reply, "elapsed": time.monotonic() - started})
            expected = "server command queued" if command.startswith("server ") else "queued"
            if reply.get("result") != expected:
                raise RuntimeError(f"Native {stage} action was not queued")
            current = wait_stage(stage, "mp_crash", current)
        report["leave_request"] = call("khz_ui leave")
        if report["leave_request"].get("result") != "queued":
            raise RuntimeError("Explicit native Leave was not queued")
        until = min(deadline, time.monotonic() + 10)
        leave = []
        report["leave_samples"] = leave
        while time.monotonic() < until:
            current = sample(until); leave.append(current)
            if not server.alive() and current["ui"].get("host", {}).get("owned") is False and current["state"].get("connection") == 0:
                report["explicit_leave_stopped_exact_server"] = True
                break
            time.sleep(.1)
        if not report.get("explicit_leave_stopped_exact_server"):
            raise RuntimeError("Explicit Leave did not stop the exact owned server and disconnect the client")
        report["passed"] = True
    except Exception as error:
        report["errors"].append(str(error))
    finally:
        report["test_elapsed_seconds"] = time.monotonic() - started
        cleanup = report["cleanup"] = {}
        if client and client.poll() is None:
            try:
                # A port can be reused after an abnormal exit. Never send a
                # cleanup command to an endpoint without checking its PID.
                endpoint = query(args.client_port, "khz_stats", "KHZ_JSON", 1)
                if endpoint.get("process_id") != client.pid:
                    raise ValueError("Cleanup endpoint is not the created client")
                if not report.get("explicit_leave_stopped_exact_server"):
                    cleanup["leave"] = query(args.client_port, "khz_ui leave", "KHZ_UI", 1)
                cleanup["quit"] = query(args.client_port, "khz_ui quit", "KHZ_UI", 1)
            except (TimeoutError, OSError, ValueError) as error:
                cleanup["query_error"] = str(error)
            try:
                client.wait(timeout=8)
            except subprocess.TimeoutExpired:
                cleanup["client_forced_stop"] = True
                client.terminate(); client.wait(timeout=5)
        cleanup["created_client_exited"] = client is None or client.poll() is not None
        cleanup["created_client_returncode"] = client.returncode if client else None
        if client and client.returncode:
            report["errors"].append(f"Created client exited with {client.returncode}")
        if server:
            try:
                if server.alive():
                    cleanup["server_forced_stop"] = True; server.stop()
                cleanup["exact_owned_server_exited"] = not server.alive()
            except Exception as error:
                report["errors"].append("Owned server cleanup: " + str(error))
                cleanup["exact_owned_server_exited"] = False
            finally:
                server.close()
        for label, directory, filename in (("client", home / "main", "khz_client.log"),
                                           ("server", server_home / "main" if server_home else output / "absent", "khz_server.log")):
            source = directory / filename
            if source.is_file():
                destination = output / (label + "-native.log")
                shutil.copy2(source, destination)
                report.setdefault("logs", {})[label] = str(destination)
        # Retain this fixture's profile/home; restore the previous benchmark home exactly.
        if fixture_home_authorized and home.exists() and cleanup["created_client_exited"]:
            home.rename(output / "fixture-client-home")
            report["fixture_home_retained"] = str(output / "fixture-client-home")
        if saved_home.exists() and not home.exists():
            saved_home.rename(home); cleanup["previous_home_restored"] = True
        if cleanup.get("client_forced_stop") or cleanup.get("server_forced_stop"):
            report["errors"].append("Native cleanup required forced termination")
        report["passed"] = bool(report["passed"] and not report["errors"] and cleanup["created_client_exited"] and cleanup.get("exact_owned_server_exited", False))
        report["elapsed_with_cleanup_seconds"] = time.monotonic() - started
        (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    print(json.dumps({"passed": report["passed"], "stages": [{"name": stage["name"], "passed": stage["passed"]} for stage in report["stages"]],
                      "errors": report["errors"], "elapsed_seconds": report["elapsed_with_cleanup_seconds"], "output": str(output / "result.json")}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
