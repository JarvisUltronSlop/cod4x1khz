"""Run only the native plugin HTTP unit executable against owned loopback HTTP."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import http.server
import json
import subprocess
import threading
import time
from pathlib import Path


class Fixture:
    def __init__(self):
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.requests: list[dict] = []
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def reply(self, code: int, body: bytes, *, headers: dict | None = None, head=False):
                self.send_response(code)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                for key, value in (headers or {}).items():
                    self.send_header(key, value)
                self.end_headers()
                if not head:
                    self.wfile.write(body)
                self.close_connection = True

            def handle_request(self):
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 <= size <= 8 * 1024 * 1024:
                    self.reply(400, b"bad length")
                    return
                body = self.rfile.read(size)
                with fixture.lock:
                    fixture.requests.append({"method": self.command, "path": self.path, "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest(), "authorization": self.headers.get("Authorization"), "fixture_header": self.headers.get("X-Native-Fixture")})
                try:
                    if self.path == "/ok":
                        self.reply(200, b"native-http-ok\n")
                    elif self.path == "/echo":
                        if self.command != "POST" or self.headers.get("Authorization") != "Bearer fixture" or self.headers.get("X-Native-Fixture") != "yes":
                            self.reply(400, b"missing explicit headers")
                        else:
                            self.reply(200, body)
                    elif self.path == "/missing":
                        self.reply(404, b"missing")
                    elif self.path == "/redirect":
                        self.reply(302, b"", headers={"Location": "/must-not-follow"})
                    elif self.path == "/head":
                        self.send_response(200)
                        self.send_header("Content-Length", str(16 * 1024 * 1024))
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.close_connection = True
                    elif self.path == "/chunked":
                        self.send_response(200)
                        self.send_header("Transfer-Encoding", "chunked")
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(b"7\r\nnative-\r\n8\r\nhttp-ok\n\r\n0\r\n\r\n")
                        self.close_connection = True
                    elif self.path == "/oversize":
                        self.send_response(200)
                        self.send_header("Content-Length", str(2 * 1024 * 1024))
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.close_connection = True
                    elif self.path == "/oversize-stream":
                        self.send_response(200)
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(b"x" * (1024 * 1024 + 1))
                        self.close_connection = True
                    elif self.path == "/truncated":
                        self.send_response(200)
                        self.send_header("Content-Length", "100")
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(b"short")
                        self.close_connection = True
                    elif self.path == "/huge-headers":
                        self.reply(200, b"x", headers={"X-Oversize": "x" * 17000})
                    elif self.path == "/slow":
                        self.send_response(200)
                        self.send_header("Content-Length", "100")
                        self.send_header("Connection", "close")
                        self.end_headers()
                        for _ in range(100):
                            if fixture.stop.wait(0.1):
                                break
                            self.wfile.write(b"x")
                            self.wfile.flush()
                        self.close_connection = True
                    else:
                        self.reply(500, b"unexpected endpoint")
                except (BrokenPipeError, ConnectionResetError, OSError):
                    self.close_connection = True

            do_GET = do_POST = do_HEAD = handle_request

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executable = args.exe.resolve(strict=True)
    root = Path(__file__).resolve().parents[1]
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = (args.output or root / "results" / f"plugin-http-{stamp}").resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixture = Fixture()
    result = {"passed": False, "scope": "Isolated native HTTP backend and public response view only; gameplay/plugin lifecycle and third-party integration remain pending.", "exe": str(executable), "exe_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    started = time.monotonic()
    child = None
    try:
        origin = f"http://127.0.0.1:{fixture.server.server_address[1]}"
        with (output / "native-tests.log").open("w", encoding="utf-8") as log:
            child = subprocess.Popen([str(executable), origin], stdout=log, stderr=subprocess.STDOUT)
            result["owned_pid"] = child.pid
            result["exit_code"] = child.wait(timeout=40)
        with fixture.lock:
            requests = list(fixture.requests)
        echo = [item for item in requests if item["path"] == "/echo"]
        expected = bytes((i * 17) & 255 for i in range(8193))
        fixture_pass = len(echo) == 1 and echo[0]["method"] == "POST" and echo[0]["sha256"] == hashlib.sha256(expected).hexdigest()
        fixture_pass = fixture_pass and not any(item["path"] == "/must-not-follow" for item in requests)
        result.update({"requests": requests, "fixture_pass": fixture_pass, "passed": result["exit_code"] == 0 and fixture_pass})
    except Exception as exc:
        result["error"] = str(exc)
    finally:
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(3)
        fixture.close()
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "result": str(output / "result.json"), "elapsed_seconds": result["elapsed_seconds"]}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
