"""Project-local loopback fixtures for native service tests; never starts a game."""
from __future__ import annotations

import argparse
import hashlib
import http.server
import io
import json
import signal
import socket
import struct
import threading
import time
import zipfile
import zlib
from pathlib import Path


def iwd_fixture() -> tuple[bytes, int, int]:
    buffer = io.BytesIO()
    crcs = bytearray()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in (("maps/mp/fixture.gsc", b"main(){wait 0.001;}\n"), ("images/fixture.iwi", b"fixture-image\x00" * 80), ("empty.txt", b"")):
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
            if data:
                crcs.extend(struct.pack("<I", zlib.crc32(data)))
    return buffer.getvalue(), zlib.crc32(crcs), zlib.crc32(crcs, 0x12345678)


class Fixture:
    def __init__(self, root: Path, http_port: int, udp_port: int):
        self.root = root
        self.asset, checksum, pure = iwd_fixture()
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.retry_count = 0
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", udp_port))
        self.udp.settimeout(0.2)
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                data = fixture.asset
                if path == "/retry.iwd":
                    with fixture.lock:
                        fixture.retry_count += 1
                        attempt = fixture.retry_count
                    if attempt < 3:
                        self.send_error(503)
                        return
                elif path == "/wrong.iwd":
                    data = data[:-1] + bytes([data[-1] ^ 1])
                elif path == "/oversize.iwd":
                    data += b"!"
                elif path == "/redirect":
                    self.send_response(302)
                    self.send_header("Location", "/asset.iwd")
                    self.end_headers()
                    return
                elif path in ("/client.bin", "/server.bin", "/manifest.txt"):
                    file = fixture.root / path[1:]
                    if not file.is_file():
                        self.send_error(404)
                        return
                    data = file.read_bytes()
                elif path == "/fixture.txt":
                    data = fixture.metadata.encode("ascii")
                elif path == "/huge-document":
                    data = b"x" * 65537
                elif path not in ("/asset.iwd", "/truncated.iwd", "/slow.iwd"):
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    if path == "/truncated.iwd":
                        self.wfile.write(data[: len(data) // 2])
                        self.close_connection = True
                    elif path == "/slow.iwd":
                        for at in range(0, len(data), 8):
                            if fixture.stop.wait(0.03):
                                break
                            self.wfile.write(data[at:at + 8])
                            self.wfile.flush()
                    else:
                        self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), Handler)
        self.http.daemon_threads = True
        self.origin = f"http://127.0.0.1:{self.http.server_port}"
        self.udp_endpoint = f"127.0.0.1:{self.udp.getsockname()[1]}"
        self.metadata = f"size={len(self.asset)}\nsha256={hashlib.sha256(self.asset).hexdigest()}\nchecksum={checksum}\npure={pure}\nfeed=305419896\n"
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / "asset.iwd").write_bytes(self.asset)
        (self.root / "fixture.txt").write_text(self.metadata, encoding="ascii", newline="\n")

    def udp_work(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as wrong_source:
            wrong_source.bind(("127.0.0.1", 0))
            while not self.stop.is_set():
                try:
                    packet, peer = self.udp.recvfrom(8192)
                except socket.timeout:
                    continue
                if packet.startswith(b"\xff" * 4 + b"getinfo "):
                    challenge = packet.split(b" ", 1)[1]
                    reply = b"\xff" * 4 + b"infoResponse\n\\challenge\\" + challenge + b"\\protocol\\10002\\hostname\\Fixture\\mapname\\mp_killhouse\\gametype\\dm\\clients\\1\\sv_maxclients\\4\\pure\\1"
                    wrong_source.sendto(reply.replace(challenge, b"wrong-challenge"), peer)
                    self.udp.sendto(reply.replace(challenge, b"wrong-challenge"), peer)
                    self.udp.sendto(reply, peer)
                elif packet.startswith(b"\xff" * 4 + b"getservers "):
                    header = b"\xff" * 4 + b"getserversResponse"
                    address = b"\\" + socket.inet_aton("127.0.0.1") + struct.pack("!H", 28961)
                    wrong_source.sendto(header + address + b"\\EOT", peer)
                    self.udp.sendto(header + address, peer)
                    self.udp.sendto(header + address + b"\\" + socket.inet_aton("127.0.0.1") + struct.pack("!H", 28962) + b"\\EOT", peer)

    def run(self):
        http_thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        udp_thread = threading.Thread(target=self.udp_work, daemon=True)
        http_thread.start()
        udp_thread.start()
        ready = {"http_origin": self.origin, "udp_endpoint": self.udp_endpoint, "data_dir": str(self.root), "scope": "isolated loopback service fixtures; no game processes"}
        (self.root / "ready.json").write_text(json.dumps(ready, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(ready), flush=True)
        try:
            while not self.stop.wait(0.2):
                pass
        finally:
            self.http.shutdown()
            self.http.server_close()
            udp_thread.join(0.5)
            self.udp.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--http-port", type=int, default=0)
    parser.add_argument("--udp-port", type=int, default=0)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    root = args.data_dir.resolve()
    if not root.is_relative_to(project) or root == project:
        parser.error("Fixture output must be in a dedicated directory beneath the project")
    fixture = Fixture(root, args.http_port, args.udp_port)
    signal.signal(signal.SIGINT, lambda *_args: fixture.stop.set())
    signal.signal(signal.SIGTERM, lambda *_args: fixture.stop.set())
    fixture.run()


if __name__ == "__main__":
    main()
