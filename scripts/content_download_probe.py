"""Short native private-pair IWD download fixture, isolated in benchmark homes."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import zipfile
import zlib
from monitor import query


def fixture_archive(marker: str, payload: bytes) -> bytes:
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        info = zipfile.ZipInfo(marker, (2020, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, payload)
    return stream.getvalue()


def validate_read(result, marker, payload):
    errors = []
    if not isinstance(result, dict):
        return ["Native filesystem reply is not an object"]
    if result.get("ok") is not True:
        errors.append("Native filesystem did not read the fixture")
    if result.get("path") != marker:
        errors.append("Native filesystem read an unexpected marker path")
    if type(result.get("bytes")) is not int or result["bytes"] != len(payload):
        errors.append("Native filesystem marker length differs")
    if type(result.get("crc")) is not int or result["crc"] != zlib.crc32(payload):
        errors.append("Native filesystem marker CRC differs")
    return errors


class ServerQueries:
    def __init__(self, port):
        self.port, self.last = port, float("-inf")

    def call(self, command, marker="KHZ_JSON"):
        delay = self.last + 1.1 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        self.last = time.monotonic()
        return query(self.port, "rcon local-khz-benchmark " + command, marker)


def available_port(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", port))


def live(processes):
    for process in processes:
        if process.poll() is not None:
            raise RuntimeError(f"Owned game process {process.pid} exited with {process.returncode}")


def native_command(arguments):
    """Keep HTTP origins quoted for the engine's Cbuf comment tokenizer."""
    result = [subprocess.list2cmdline([arguments[0]])]
    for value in arguments[1:]:
        if "//" in value:
            if '"' in value or any(char.isspace() for char in value):
                raise ValueError("Unexpected origin token")
            result.append('"' + value + '"')
        else:
            result.append(subprocess.list2cmdline([value]))
    return " ".join(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-port", type=int, default=28983)
    parser.add_argument("--client-port", type=int, default=28984)
    parser.add_argument("--qport", type=int, default=28984, help="Native wire qport; use32768/65535 for signed-boundary regression")
    parser.add_argument("--map", choices=("mp_killhouse", "mp_crash"), default="mp_killhouse")
    parser.add_argument("--timeout", type=int, default=35)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        data = b"fixture bytes\n"
        valid = {"ok": True, "path": "khz_fixture_mock.bin", "bytes": len(data), "crc": zlib.crc32(data)}
        assert not validate_read(valid, valid["path"], data)
        assert validate_read({}, valid["path"], data)
        assert validate_read({**valid, "crc": valid["crc"] ^ 1}, valid["path"], data)
        assert validate_read({**valid, "bytes": True}, valid["path"], data)
        image = fixture_archive(valid["path"], data)
        with zipfile.ZipFile(io.BytesIO(image)) as archive:
            assert archive.namelist() == [valid["path"]] and archive.read(valid["path"]) == data
        print("Native-content fixture schema/archive self-tests passed")
        return 0
    if not all(1024 <= port <= 65535 for port in (args.server_port, args.client_port)) or args.server_port == args.client_port or not 5 <= args.timeout <= 60:
        parser.error("Distinct ports must be 1024..65535 and timeout 5..60 seconds")
    if not 1 <= args.qport <= 65535:
        parser.error("Qport must be1..65535")
    project = Path(__file__).resolve().parent.parent
    run = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f") + f"-{os.getpid()}"
    output = (args.output or project / "results" / ("native-content-" + run)).resolve()
    if not output.is_relative_to(project / "results") or output == project / "results" or output.exists():
        parser.error("Output must be a new dedicated results directory")
    server_binary = project / "runtime/cod4x1khz_server.exe"
    client_binary = project / "runtime/cod4x1khz.exe"
    if not server_binary.is_file() or not client_binary.is_file():
        parser.error("Build the native custom pair first")
    for port in (args.server_port, args.client_port):
        available_port(port)
    output.mkdir(parents=True)
    marker = "khz_fixture_" + run + ".bin"
    relative = "main/khz_fixture_" + run + ".iwd"
    payload = ("Native cod4x1khz content fixture: " + marker + "\n").encode("ascii")
    archive = fixture_archive(marker, payload)
    archive_hash = hashlib.sha256(archive).hexdigest()
    server_home = project / "runtime/benchmarks" / f"server-{args.server_port}"
    client_home = project / "runtime/benchmarks" / f"client-{args.client_port}"
    server_file, client_file = server_home / relative, client_home / relative
    for file in (server_file, client_file, project / "runtime" / relative):
        if file.exists():
            parser.error("Fixture must not overwrite or reuse an existing archive")
    server_file.parent.mkdir(parents=True, exist_ok=True)
    server_file.write_bytes(archive)
    requests = []
    request_lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_items):
            pass

        def do_GET(self):
            with request_lock:
                requests.append({"path": self.path, "peer": self.client_address[0], "time": time.monotonic()})
            if self.path != "/" + relative or self.client_address[0] != "127.0.0.1":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(archive)))
            self.end_headers()
            self.wfile.write(archive)

    http_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    http_server.daemon_threads = True
    thread = threading.Thread(target=http_server.serve_forever, daemon=True)
    thread.start()
    origin = f"http://127.0.0.1:{http_server.server_port}"
    processes = []
    report = {"map": args.map, "server_port": args.server_port, "client_port": args.client_port, "qport": args.qport,
              "archive": relative, "marker": marker, "archive_sha256": archive_hash,
              "marker_crc": zlib.crc32(payload), "errors": [], "scope":
              "One actual accepted-session native IWD metadata/download/commit/remount/pure-join fixture. "
              "Archive exists only in the owned server benchmark home before transfer. "
              "Stock assets and interactive profiles are untouched. This does not establish fastfile, retry/cancel, internet authentication or full CoD4x download parity."}
    started = time.monotonic()
    try:
        report["binary_sha256"] = {"client": hashlib.sha256(client_binary.read_bytes()).hexdigest(), "server": hashlib.sha256(server_binary.read_bytes()).hexdigest()}
        server_args = [str(server_binary), "+set", "dedicated", "1", "+set", "net_ip", "127.0.0.1", "+set", "net_port", str(args.server_port),
                       "+set", "khz_benchmark", "1", "+set", "khz_test_controls", "1", "+exec", "server.cfg",
                       "+set", "khz_bots", "0", "+set", "khz_bot_count", "0", "+set", "sv_allowDownload", "1",
                       "+set", "sv_wwwBaseURL", origin, "+set", "sv_pure", "1", "+map", args.map]
        processes.append(subprocess.Popen(native_command(server_args), cwd=project / "runtime", creationflags=subprocess.CREATE_NO_WINDOW))
        server = ServerQueries(args.server_port)
        deadline = time.monotonic() + args.timeout
        while True:
            live(processes)
            try:
                report["before_server"] = server.call("khz_json")
                if report["before_server"].get("process_id") != processes[0].pid:
                    raise RuntimeError("Server telemetry belongs to a different process")
                break
            except (TimeoutError, OSError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Owned server did not initialize before timeout")
        report["reference"] = server.call("khz_content_fixture_reference " + marker, "KHZ_CONTENT_FIXTURE")
        report["errors"].extend(validate_read(report["reference"], marker, payload))
        if report["errors"]:
            raise RuntimeError("Server failed actual IWD marker reference")
        client_args = [str(client_binary), "+set", "net_port", str(args.client_port), "+set", "khz_benchmark", "1",
                       "+set", "khz_test_controls", "1", "+set", "com_introPlayed", "1", "+set", "r_customMode", "1280x720", "+set", "r_mode", "1280x720",
                       "+set", "r_fullscreen", "0", "+exec", "client.cfg", "+set", "com_maxfps", "0",
                       "+set", "cl_allowDownload", "1", "+set", "khz_download_origin", origin,
                       "+setqport", str(args.qport), "+getqport", "+connect", f"127.0.0.1:{args.server_port}"]
        processes.append(subprocess.Popen(native_command(client_args), cwd=project / "runtime"))
        report["owned_pids"] = [process.pid for process in processes]
        deadline = time.monotonic() + args.timeout
        samples = []
        while time.monotonic() < deadline:
            live(processes)
            try:
                sample = {"stats": query(args.client_port, "khz_stats"), "state": query(args.client_port, "khz_state", "KHZ_STATE")}
                if sample["stats"].get("process_id") != processes[1].pid:
                    raise RuntimeError("Client telemetry belongs to a different process")
                samples.append(sample)
                if sample["stats"].get("services_client", {}).get("content_rejected", 0):
                    report["client"] = sample
                    raise RuntimeError("Native content adapter rejected the transfer; see copied client log")
                if sample["state"].get("connection") == 9:
                    report["client"] = sample
                    break
            except (TimeoutError, OSError):
                pass
            time.sleep(0.2)
        report["samples"] = samples
        if "client" not in report:
            raise RuntimeError("Client did not complete the actual content/pure join before timeout")
        state = report["client"]["state"]
        if not state.get("server_id"):
            raise RuntimeError("Joined client has no accepted map generation")
        services = report["client"]["stats"].get("services_client", {})
        report["services_client"] = services
        if not services.get("available") or services.get("content_pending") is not False or services.get("content_requests", 0) < 1 or services.get("content_completed", 0) < 1 or services.get("content_committed", 0) < 1 or services.get("content_rejected") != 0:
            raise RuntimeError("Actual native content request/complete/commit counters did not prove the transfer")
        if not client_file.is_file() or hashlib.sha256(client_file.read_bytes()).hexdigest() != archive_hash:
            raise RuntimeError("Client home does not contain the exact SHA-verified fixture archive")
        report["read"] = query(args.client_port, "khz_content_read " + marker, "KHZ_CONTENT_FIXTURE", timeout=3)
        report["errors"].extend(validate_read(report["read"], marker, payload))
        report["after_server"] = server.call("khz_json")
        if report["after_server"].get("process_id") != processes[0].pid:
            raise RuntimeError("Server process changed during the fixture")
        server_content = report["after_server"].get("content_server", {})
        report["content_server"] = server_content
        if any(server_content.get(field, 0) < 1 for field in ("accepted", "queued", "hashed", "sent")) or server_content.get("errors") != 0:
            report["errors"].append("Native server metadata worker counters did not prove one clean admission/hash/send")
        if not any(peer.get("port") == args.client_port for peer in report["after_server"].get("peers", [])):
            report["errors"].append("Server did not acknowledge the owned client as active after the pure join")
        with request_lock:
            report["http_requests"] = list(requests)
        if len(report["http_requests"]) != 1 or report["http_requests"][0]["path"] != "/" + relative:
            report["errors"].append("Fixture HTTP did not receive exactly the authorized native transfer")
    except (OSError, TimeoutError, ValueError, RuntimeError, KeyError, TypeError) as error:
        report["errors"].append(str(error))
    finally:
        with request_lock:
            report["http_requests"] = list(requests)
        if processes and processes[0].poll() is None and "server" in locals():
            try:
                report["final_server"] = server.call("khz_json")
            except (OSError, TimeoutError, ValueError):
                pass
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        http_server.shutdown()
        http_server.server_close()
        thread.join(1)
        for name, home in (("server", server_home), ("client", client_home)):
            source = home / "main" / f"khz_{name}.log"
            if source.is_file():
                (output / f"native-{name}.log").write_bytes(source.read_bytes())
        for file in (server_file, client_file):
            if file.exists():
                if file.is_file() and hashlib.sha256(file.read_bytes()).hexdigest() == archive_hash:
                    file.unlink()
                else:
                    report["errors"].append("Changed fixture archive was preserved for inspection: " + str(file))
        report["elapsed_seconds"] = time.monotonic() - started
        report["passed"] = "read" in report and not report["errors"]
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report.get(key) for key in ("passed", "elapsed_seconds", "errors")}, indent=2))
        print(str(output / "result.json"))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
