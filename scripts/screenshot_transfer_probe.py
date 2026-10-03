"""Short actual private-pair getss transfer; isolated benchmark homes only."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
from content_download_probe import ServerQueries, available_port, live, native_command
from monitor import query


def jpeg_size(data):
    """Bounded JPEG marker inspection; the native receiver fully decodes WIC."""
    if len(data) < 4 or len(data) > 1024 * 1024 or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise ValueError("Saved screenshot is not a bounded complete JPEG")
    at = 2
    while at < len(data):
        if data[at] != 255:
            raise ValueError("Malformed JPEG marker")
        while at < len(data) and data[at] == 255:
            at += 1
        if at >= len(data):
            break
        marker = data[at]
        at += 1
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if at + 2 > len(data):
            break
        length = struct.unpack_from(">H", data, at)[0]
        if length < 2 or at + length > len(data):
            raise ValueError("Malformed JPEG segment length")
        if marker in (0xC0, 0xC1, 0xC2):
            if length < 8:
                raise ValueError("Malformed JPEG dimensions")
            height, width = struct.unpack_from(">HH", data, at + 3)
            if not 1 <= width <= 1280 or not 1 <= height <= 720:
                raise ValueError("Saved screenshot dimensions exceed transfer contract")
            return width, height
        if marker == 0xDA:
            break
        at += length
    raise ValueError("Saved screenshot has no supported JPEG frame")


def counters(stats, field):
    value = stats.get(field)
    if not isinstance(value, dict) or value.get("initialized") is not True:
        raise RuntimeError(f"Missing native {field} telemetry")
    return value


def delta(before, after, field):
    first, last = before.get(field), after.get(field)
    if type(first) is not int or type(last) is not int or first < 0 or last < first:
        raise RuntimeError(f"Invalid or reset {field} counter")
    return last - first


def fire(server, command):
    delay = server.last + 1.1 - time.monotonic()
    if delay > 0:
        time.sleep(delay)
    server.last = time.monotonic()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(b"\xff" * 4 + ("rcon local-khz-benchmark " + command).encode("ascii"), ("127.0.0.1", server.port))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-port", type=int, default=28985)
    parser.add_argument("--client-port", type=int, default=28986)
    parser.add_argument("--map", choices=("mp_killhouse", "mp_crash"), default="mp_killhouse")
    parser.add_argument("--timeout", type=int, default=35)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        fixture = b"\xff\xd8\xff\xc0\x00\x0b\x08\x02\xd0\x05\x00\x01\x01\x11\x00\xff\xd9"
        assert jpeg_size(fixture) == (1280, 720)
        for bad in (b"", b"\xff\xd8\xff\xd9", fixture[:-1], fixture[:10]):
            try:
                jpeg_size(bad)
            except ValueError:
                pass
            else:
                raise AssertionError("Malformed JPEG marker fixture passed")
        assert delta({"x": 1}, {"x": 3}, "x") == 2
        for before, after in (({}, {"x": 1}), ({"x": True}, {"x": 1}), ({"x": 2}, {"x": 1})):
            try:
                delta(before, after, "x")
            except RuntimeError:
                pass
            else:
                raise AssertionError("Invalid counter fixture passed")
        print("Screenshot probe schema/JPEG marker fixtures passed; native full-decode proof is separate")
        return 0
    if not 5 <= args.timeout <= 60 or args.server_port == args.client_port or not all(1024 <= p <= 65535 for p in (args.server_port, args.client_port)):
        parser.error("Use distinct ports1024..65535 and a timeout5..60 seconds")
    project = Path(__file__).resolve().parent.parent
    run = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f") + f"-{os.getpid()}"
    output = (args.output or project / "results" / ("native-screenshot-" + run)).resolve()
    if output.exists() or not output.is_relative_to(project / "results") or output == project / "results":
        parser.error("Output must be a new dedicated results directory")
    server_binary, client_binary = project / "runtime/cod4x1khz_server.exe", project / "runtime/cod4x1khz.exe"
    if not server_binary.is_file() or not client_binary.is_file():
        parser.error("Build the custom native pair first")
    for port in (args.server_port, args.client_port):
        available_port(port)
    output.mkdir(parents=True)
    basename = "khz_fixture_ss_" + datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S") + "_" + str(os.getpid())
    home = project / "runtime/benchmarks" / f"server-{args.server_port}"
    report = {"errors": [], "map": args.map, "basename": basename, "scope":
              "One actual accepted private-pair getss request, backbuffer capture, worker JPEG/SHA, "
              "bounded reliable chunk transfer and native admission-bound server save. "
              "Does not prove authenticated public screenshot attestation, arbitrary public client compatibility, "
              "getss all fairness, adverse-network recovery or an external screenshot program callback."}
    processes = []
    started = time.monotonic()
    try:
        report["binary_sha256"] = {"client": hashlib.sha256(client_binary.read_bytes()).hexdigest(), "server": hashlib.sha256(server_binary.read_bytes()).hexdigest()}
        server_args = [str(server_binary), "+set", "dedicated", "1", "+set", "net_ip", "127.0.0.1", "+set", "net_port", str(args.server_port),
                       "+set", "khz_benchmark", "1", "+set", "khz_test_controls", "1", "+exec", "server.cfg",
                       "+set", "khz_bots", "0", "+set", "khz_bot_count", "0", "+map", args.map]
        processes.append(subprocess.Popen(native_command(server_args), cwd=project / "runtime", creationflags=subprocess.CREATE_NO_WINDOW))
        server = ServerQueries(args.server_port)
        until = time.monotonic() + args.timeout
        while True:
            live(processes)
            try:
                before = server.call("khz_json")
                if before.get("process_id") != processes[0].pid:
                    raise RuntimeError("Server endpoint belongs to another process")
                if before.get("ticks", 0) > 0:
                    break
            except (TimeoutError, OSError):
                pass
            if time.monotonic() >= until:
                raise RuntimeError("Owned server did not initialize")
        client_args = [str(client_binary), "+set", "net_port", str(args.client_port), "+set", "khz_benchmark", "1", "+set", "khz_test_controls", "1", "+set", "com_introPlayed", "1",
                       "+set", "r_customMode", "1280x720", "+set", "r_mode", "1280x720", "+set", "r_fullscreen", "0", "+exec", "client.cfg",
                       "+set", "com_maxfps", "0", "+setqport", "28986", "+connect", f"127.0.0.1:{args.server_port}"]
        processes.append(subprocess.Popen(native_command(client_args), cwd=project / "runtime"))
        report["owned_pids"] = [p.pid for p in processes]
        samples = []
        until = time.monotonic() + args.timeout
        while time.monotonic() < until:
            live(processes)
            try:
                sample = query(args.client_port, "khz_stats")
                state = query(args.client_port, "khz_state", "KHZ_STATE")
                if sample.get("process_id") != processes[1].pid:
                    raise RuntimeError("Client endpoint belongs to another process")
                samples.append({"stats": sample, "state": state})
                owner = sample.get("client_owner", {})
                if state.get("connection") == 9 and owner.get("active") is True and owner.get("ticks", 0) > 0:
                    report["before_client"] = sample
                    break
            except (TimeoutError, OSError):
                pass
            time.sleep(.1)
        report["join_samples"] = samples
        if "before_client" not in report:
            raise RuntimeError("Accepted native gameplay owner did not start before timeout")
        report["before_server"] = server.call("khz_json")
        cs = counters(report["before_client"], "screenshot_client")
        ss = counters(report["before_server"], "screenshot_server")
        candidates = [p for p in report["before_server"].get("peers", []) if p.get("port") == args.client_port]
        if len(candidates) != 1 or type(candidates[0].get("client")) is not int:
            raise RuntimeError("Server has no unique accepted owned client slot")
        slot = candidates[0]["client"]
        fire(server, f"getss {slot} {basename}")
        until = time.monotonic() + args.timeout
        transfer = []
        while time.monotonic() < until:
            live(processes)
            sample = query(args.client_port, "khz_stats")
            if sample.get("process_id") != processes[1].pid:
                raise RuntimeError("Client changed during screenshot transfer")
            transfer.append(sample)
            if delta(cs, counters(sample, "screenshot_client"), "completed") >= 1:
                report["after_client"] = sample
                break
            time.sleep(.1)
        report["transfer_samples"] = transfer
        if "after_client" not in report:
            raise RuntimeError("Native screenshot transfer did not complete before timeout")
        report["after_server"] = server.call("khz_json")
        after_cs, after_ss = counters(report["after_client"], "screenshot_client"), counters(report["after_server"], "screenshot_server")
        report["client_deltas"] = {k: delta(cs, after_cs, k) for k in ("requests", "captured", "encoded", "chunks", "completed", "errors", "rejected", "queue_drops")}
        report["server_deltas"] = {k: delta(ss, after_ss, k) for k in ("requested", "queued", "sent", "metadata", "chunks", "verified", "saved", "errors", "rejected", "timeouts")}
        if any(report["client_deltas"][key] != 1 for key in ("requests", "captured", "encoded", "completed")) or report["client_deltas"]["chunks"] < 1:
            raise RuntimeError("Client counters did not prove one actual screenshot")
        if any(report["server_deltas"][key] != 1 for key in ("requested", "queued", "sent", "metadata", "verified", "saved")) or report["server_deltas"]["chunks"] != report["client_deltas"]["chunks"]:
            raise RuntimeError("Server counters did not prove exact native verification/save")
        if any(report["client_deltas"][key] for key in ("errors", "rejected", "queue_drops")) or any(report["server_deltas"][key] for key in ("errors", "rejected", "timeouts")):
            raise RuntimeError("Screenshot transfer reported a native error or queue drop")
        files = list((home / "screenshots").glob(basename + "-*.jpg"))
        if len(files) != 1:
            raise RuntimeError("Native receiver did not save exactly the owned screenshot")
        data = files[0].read_bytes()
        width, height = jpeg_size(data)
        saved = output / "screenshot.jpg"
        saved.write_bytes(data)
        report["image"] = {"native_path": str(files[0]), "result_path": str(saved), "width": width, "height": height,
                           "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        first, last = report["before_client"], report["after_client"]
        duration = last["seconds"] - first["seconds"]
        report["simulation_during_transfer"] = {"seconds": duration, "owner_ticks": delta(first["client_owner"], last["client_owner"], "ticks"),
            "commands_hz": delta(first, last, "commands_sent") / duration, "snapshots_hz": delta(first, last, "snapshots_received") / duration,
            "prediction_hz": delta(first, last, "prediction_ticks") / duration,
            "short_duration_note": "Continuity diagnostic only; this single transfer is too short to establish sustained1kHz acceptance."}
        if first["client_owner"].get("generation") != last["client_owner"].get("generation") or report["simulation_during_transfer"]["owner_ticks"] <= 0:
            raise RuntimeError("Gameplay owner restarted or stopped during screenshot transfer")
    except (OSError, TimeoutError, ValueError, RuntimeError, KeyError, TypeError, ZeroDivisionError) as error:
        report["errors"].append(str(error))
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for name, port in (("server", args.server_port), ("client", args.client_port)):
            log = project / "runtime/benchmarks" / f"{name}-{port}" / "main" / f"khz_{name}.log"
            if log.is_file():
                (output / f"native-{name}.log").write_bytes(log.read_bytes())
        report["elapsed_seconds"] = time.monotonic() - started
        report["passed"] = "image" in report and not report["errors"]
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report.get(key) for key in ("passed", "elapsed_seconds", "errors")}, indent=2))
        print(output / "result.json")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
