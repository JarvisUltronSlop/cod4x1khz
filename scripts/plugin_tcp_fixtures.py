"""Owned loopback-only fixtures for the standalone plugin TCP backend."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import socket
import socketserver
import subprocess
import threading
import time


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        received = sent = 0
        close_command = False
        first = bytearray()
        decided = False
        self.request.settimeout(.2)
        try:
            while not self.server.stopped.is_set():
                try:
                    data = self.request.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                received += len(data)
                if not decided:
                    first.extend(data)
                    if len(first) < 6:
                        continue
                    if first[:6] == b"CLOSE\n":
                        close_command = True
                        break
                    data = bytes(first)
                    first.clear()
                    decided = True
                self.request.sendall(data)
                sent += len(data)
        except OSError:
            pass
        finally:
            with self.server.events_lock:
                self.server.events.append({"peer": self.client_address[0], "received": received,
                                           "sent": sent, "orderly_close_command": close_command})


class Fixture(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, family):
        self.address_family = family
        self.events = []
        self.events_lock = threading.Lock()
        self.stopped = threading.Event()
        super().__init__(("::1" if family == socket.AF_INET6 else "127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": .05}, daemon=True)
        self.thread.start()

    def close(self):
        self.stopped.set()
        self.shutdown()
        self.server_close()
        self.thread.join(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    ipv4 = Fixture(socket.AF_INET)
    ipv6 = None
    process = None
    try:
        if args.self_test:
            with socket.create_connection(ipv4.server_address, timeout=1) as sock:
                payload = b"binary\x00fixture"
                sock.sendall(payload)
                received = bytearray()
                while len(received) < len(payload):
                    chunk = sock.recv(1024)
                    assert chunk
                    received.extend(chunk)
                assert received == payload
            with socket.create_connection(ipv4.server_address, timeout=1) as sock:
                sock.sendall(b"CLOSE\n")
                assert sock.recv(1) == b""
            print("Owned TCP echo/orderly-close fixture self-tests passed")
            return 0
        if not args.binary or not args.binary.is_file():
            parser.error("Pass the built standalone TCP test executable with --binary")
        try:
            ipv6 = Fixture(socket.AF_INET6)
        except OSError:
            pass
        project = Path(__file__).resolve().parent.parent
        run = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
        output = project / "results" / ("plugin-tcp-" + run)
        output.mkdir()
        arguments = [str(args.binary.resolve()), f"127.0.0.1:{ipv4.server_address[1]}", f"localhost:{ipv4.server_address[1]}"]
        if ipv6:
            arguments.append(f"[::1]:{ipv6.server_address[1]}")
        report = {"binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "errors": [],
                  "ipv6_available": ipv6 is not None, "scope": "actual bounded native TCP backend against owned loopback services; plugin wrapper/unload/public application semantics require separate native fixtures"}
        began = time.monotonic()
        try:
            process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            report["owned_pid"] = process.pid
            stdout, _ = process.communicate(timeout=25)
            text = stdout.decode("utf-8", "replace")
            (output / "native-tests.log").write_text(text, encoding="utf-8")
            lines = [line.partition(" ")[2] for line in text.splitlines() if line.startswith("KHZ_PLUGIN_TCP_TEST ")]
            if len(lines) != 1:
                raise RuntimeError("Native TCP test result marker missing/duplicated")
            report["native"] = json.loads(lines[0])
            if process.returncode != 0 or report["native"].get("failures") != 0 or report["native"].get("network_fixture") is not True:
                raise RuntimeError("Standalone native TCP fixture tests failed")
            if ipv6 and report["native"].get("ipv6_fixture") is not True:
                raise RuntimeError("Available IPv6 fixture was not exercised")
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            report["errors"].append(str(error))
        finally:
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            ipv4.close()
            if ipv6:
                ipv6.close()
            report["elapsed_seconds"] = time.monotonic() - began
            with ipv4.events_lock:
                report["ipv4_events"] = list(ipv4.events)
            if ipv6:
                with ipv6.events_lock:
                    report["ipv6_events"] = list(ipv6.events)
            report["passed"] = "native" in report and not report["errors"]
            (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(json.dumps({key: report[key] for key in ("passed", "elapsed_seconds", "errors")}, indent=2))
            print(output / "result.json")
        return 0 if report["passed"] else 1
    finally:
        if ipv4.thread.is_alive():
            ipv4.close()
        if ipv6 and ipv6.thread.is_alive():
            ipv6.close()


if __name__ == "__main__":
    raise SystemExit(main())
