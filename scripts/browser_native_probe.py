"""Native menu-client private master/browser/favorites fixture; no gameplay run."""
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
import threading
import time
from content_download_probe import native_command
from monitor import query


def cache(data):
    record_size, global_size, favorite_size = 148, 0x2D2A80, 0x4A00
    if len(data) != 16 + global_size + favorite_size:
        raise ValueError("Native stock server cache has an unexpected length")
    version, global_count, favorites_count, size = struct.unpack_from("<iiii", data)
    if version != 1 or size != global_size + favorite_size or not 0 <= global_count <= 1024 or not 0 <= favorites_count <= 128:
        raise ValueError("Native stock server cache header is invalid")

    def records(base, count):
        result = []
        for index in range(count):
            record = data[base + index * record_size:base + (index + 1) * record_size]

            def string(at, length):
                value = record[at:at + length]
                if 0 not in value:
                    raise ValueError("Native browser string is not terminated")
                return value.split(b"\x00", 1)[0].decode("ascii")

            result.append({"type": struct.unpack_from("<i", record)[0], "ip": socket.inet_ntoa(record[4:8]),
                           "port": struct.unpack_from("!H", record, 8)[0], "pure": bool(record[22]),
                           "clients": record[25], "maximum": record[26], "request_count": record[34],
                           "ping": struct.unpack_from("<h", record, 40)[0],
                           "hostname": string(42, 32), "map": string(74, 32), "mod": string(106, 24), "game_type": string(130, 16)})
        return result

    return {"global": records(16, global_count), "favorites": records(16 + global_size, favorites_count)}


class Fixture:
    def __init__(self):
        self.master, self.server, self.spoof = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(3)]
        for sock in (self.master, self.server, self.spoof):
            sock.bind(("127.0.0.1", 0))
            sock.settimeout(.1)
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.requests = []
        self.threads = [threading.Thread(target=self.work, args=(sock,), daemon=True) for sock in (self.master, self.server)]
        for thread in self.threads:
            thread.start()

    def work(self, sock):
        while not self.stop.is_set():
            try:
                packet, peer = sock.recvfrom(8192)
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.requests.append({"socket": "master" if sock is self.master else "server", "packet": packet[4:].decode("ascii", "replace"), "peer": peer[0]})
            if sock is self.master and packet.startswith(b"\xff" * 4 + b"getservers "):
                record = b"\\" + socket.inet_aton("127.0.0.1") + struct.pack("!H", self.server.getsockname()[1])
                header = b"\xff" * 4 + b"getserversResponse"
                self.spoof.sendto(header + b"\\" + socket.inet_aton("127.0.0.1") + struct.pack("!H", 1) + b"\\EOT", peer)
                sock.sendto(header + record + b"\\EOT", peer)
            elif sock is self.server and packet.startswith(b"\xff" * 4 + b"getinfo "):
                challenge = packet.split(b" ", 1)[1]
                reply = b"\xff" * 4 + b"infoResponse\n\\challenge\\" + challenge + b"\\protocol\\10002\\hostname\\KHZNativeBrowserFixture\\mapname\\mp_killhouse\\gametype\\dm\\clients\\1\\sv_maxclients\\4\\pure\\1"
                self.spoof.sendto(reply.replace(b"KHZNativeBrowserFixture", b"WrongSource"), peer)
                sock.sendto(reply.replace(challenge, b"wrong-challenge").replace(b"KHZNativeBrowserFixture", b"WrongChallenge"), peer)
                sock.sendto(reply, peer)

    def close(self):
        self.stop.set()
        for thread in self.threads:
            thread.join(.5)
        for sock in (self.master, self.server, self.spoof):
            sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=int, default=20)
    parser.add_argument("--intro-played", type=int, choices=(0, 1), default=1,
                        help="Benchmark-only startup setting; normal MP still opens directly into its menu")
    parser.add_argument("--require-cinematic-shell", action="store_true",
                        help="Explicitly play stock IW_logo and require browser publication during cinematic/logo")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        fixture = bytearray(16 + 0x2D2A80 + 0x4A00)
        struct.pack_into("<iiii", fixture, 0, 1, 1, 1, 0x2D2A80 + 0x4A00)
        for at in (16, 16 + 0x2D2A80):
            struct.pack_into("<i", fixture, at, 4)
            fixture[at + 4:at + 8] = socket.inet_aton("127.0.0.1")
            struct.pack_into("!H", fixture, at + 8, 28960)
            fixture[at + 42:at + 49] = b"Fixture"
        result = cache(fixture)
        assert result["global"][0]["hostname"] == "Fixture" and result["favorites"][0]["port"] == 28960
        try:
            cache(fixture[:-1])
        except ValueError:
            pass
        else:
            raise AssertionError("Truncated native cache accepted")
        print("Native browser cache parser self-tests passed")
        return 0
    if not 5 <= args.timeout <= 45:
        parser.error("Use a timeout5..45 seconds")
    project = Path(__file__).resolve().parent.parent
    binary = project / "runtime/cod4x1khz.exe"
    if not binary.is_file():
        parser.error("Build the native client first")
    for _ in range(32):
        reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
        home = project / "runtime/benchmarks" / f"client-{port}"
        reservation.close()
        if not home.exists():
            break
    else:
        parser.error("Could not reserve a fresh benchmark home")
    run = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f") + f"-{os.getpid()}"
    output = (args.output or project / "results" / ("native-browser-" + run)).resolve()
    if output.exists() or not output.is_relative_to(project / "results") or output == project / "results":
        parser.error("Output must be a new dedicated results directory")
    output.mkdir(parents=True)
    fixture = Fixture()
    endpoint = "127.0.0.1:" + str(fixture.server.getsockname()[1])
    (home / "services").mkdir(parents=True)
    favorite = home / "services/favorites.txt"
    favorite.write_text("KHZ_FAVORITES_1\n" + endpoint + "\tNativeFixtureFavorite\n", encoding="ascii", newline="\n")
    process = None
    report = {"errors": [], "port": port, "benchmark_home": str(home), "endpoint": endpoint,
              "requested_intro_played": args.intro_played, "require_cinematic_shell": args.require_cinematic_shell,
              "scope": "Actual native private master worker -> stock browser arrays/cache plus native favorites import/save. "
                       "Wrong source and wrong challenge fixture packets accompany the valid query. "
                       "Public authenticated masters, internet services, join-selection UI and full CoD4x browser parity remain separate gates."}
    started = time.monotonic()
    try:
        report["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        arguments = [str(binary), "+set", "net_port", str(port), "+set", "khz_benchmark", "1", "+set", "khz_test_controls", "1", "+set", "com_introPlayed", str(args.intro_played),
                     "+set", "r_customMode", "1280x720", "+set", "r_mode", "1280x720", "+set", "r_fullscreen", "0",
                     "+exec", "client.cfg", "+set", "com_maxfps", "0", "+set", "khz_master_servers",
                     "127.0.0.1:" + str(fixture.master.getsockname()[1]), "+set", "ui_netSource", "1", "+globalservers"]
        if args.require_cinematic_shell:
            arguments.extend(("+cinematic", "IW_logo"))
            report["explicit_fixture_cinematic"] = "IW_logo"
        process = subprocess.Popen(native_command(arguments), cwd=project / "runtime")
        report["owned_pid"] = process.pid
        until = time.monotonic() + args.timeout
        samples = []
        refresh_at = None
        while time.monotonic() < until:
            if process.poll() is not None:
                raise RuntimeError(f"Owned client exited with {process.returncode}")
            try:
                sample = query(port, "khz_stats")
                if sample.get("process_id") != process.pid:
                    raise RuntimeError("Browser endpoint belongs to another process")
                samples.append(sample)
                if "menu" not in report:
                    opened = query(port, "khz_ui menu pc_join_unranked", "KHZ_UI")
                    if opened.get("result") == "opened":
                        report["menu"] = opened
                        items = []
                        offset = 0
                        while offset < 512:
                            inspected = query(port, f"khz_ui inspect pc_join_unranked {offset}", "KHZ_UI")
                            if inspected.get("result") != "inspected":
                                raise RuntimeError("Stock browser item inspection failed")
                            items.extend(inspected.get("items", []))
                            offset += 16
                            if offset >= inspected.get("item_count", 0):
                                break
                        report["stock_browser_items"] = items
                        refresh = [item for item in items if "REFRESH" in (item.get("text", "") + item.get("name", "")).upper()
                                   and item.get("action") is True and item.get("visible") is True and item.get("enabled") is True]
                        if refresh:
                            report["refresh"] = query(port, f"khz_ui activate pc_join_unranked {refresh[0]['index']}", "KHZ_UI")
                            if report["refresh"].get("result") != "stock action executed":
                                raise RuntimeError("Native stock browser refresh action failed")
                            refresh_at = time.monotonic()
                with fixture.lock:
                    observed = list(fixture.requests)
                if any(r["socket"] == "master" for r in observed) and any(r["socket"] == "server" for r in observed):
                    service = sample.get("services_client", {})
                    # Queue counters exclude jobs already owned by a worker.
                    # A refresh also invalidates an earlier startup query's
                    # epoch. Allow that final native refresh to finish before
                    # normal quit persists its arrays; the cache remains the
                    # strict proof, rather than these transient queue counts.
                    if refresh_at is not None and time.monotonic() - refresh_at >= 3 and service.get("browser_jobs") == 0 and service.get("browser_results") == 0:
                        break
            except (TimeoutError, OSError):
                pass
            time.sleep(.1)
        report["samples"] = samples
        with fixture.lock:
            report["requests"] = list(fixture.requests)
        if not samples or not any(r["socket"] == "master" for r in report["requests"]) or not any(r["socket"] == "server" for r in report["requests"]):
            raise RuntimeError("Native master/info workers did not contact the controlled endpoints")
        if report.get("menu", {}).get("result") != "opened":
            raise RuntimeError("Stock Join Server menu could not open")
        time.sleep(.2)
        report["last_stats"] = query(port, "khz_stats")
        services = report["last_stats"].get("services_client", {})
        if services.get("available") is not True or services.get("browser_failures") != 0 or services.get("favorite_failures") != 0:
            raise RuntimeError("Native browser/favorites service reported a failure")
        shell_samples = [sample.get("services_client", {}) for sample in samples + [report["last_stats"]]]
        report["cinematic_shell_publication"] = any(
            service.get("native_connection_state") in (1, 2) and service.get("browser_global_count", 0) == 1
            and service.get("browser_favorite_count", 0) == 1 for service in shell_samples)
        if args.require_cinematic_shell and not report["cinematic_shell_publication"]:
            raise RuntimeError("No native cinematic/logo phase published the verified server and favorite")
        report["quit"] = query(port, "khz_ui quit", "KHZ_UI")
        process.wait(timeout=10)
        if process.returncode != 0:
            raise RuntimeError(f"Native normal quit failed with {process.returncode}")
        native = home / "servercache.dat"
        if not native.is_file():
            raise RuntimeError("Native UI did not persist its stock browser arrays")
        data = native.read_bytes()
        (output / "native-servercache.dat").write_bytes(data)
        report["cache"] = cache(data)
        expected_port = fixture.server.getsockname()[1]
        servers = report["cache"]["global"]
        if len(servers) != 1 or servers[0]["ip"] != "127.0.0.1" or servers[0]["port"] != expected_port or servers[0]["hostname"] != "KHZNativeBrowserFixture":
            raise RuntimeError("Stock browser arrays/cache did not contain exactly the source/challenge-verified fixture server")
        if servers[0]["map"] != "mp_killhouse" or servers[0]["game_type"] != "dm" or servers[0]["clients"] != 1 or servers[0]["maximum"] != 4 or servers[0]["ping"] <= 0:
            raise RuntimeError("Stock native browser fields differ from the actual info reply")
        favorites = report["cache"]["favorites"]
        if len(favorites) != 1 or favorites[0]["port"] != expected_port or favorites[0]["hostname"] != "NativeFixtureFavorite":
            raise RuntimeError("Stock favorites arrays did not import the configured native service favorite")
        report["favorites_text"] = favorite.read_text(encoding="ascii")
        if endpoint not in report["favorites_text"] or "NativeFixtureFavorite" not in report["favorites_text"]:
            raise RuntimeError("Native favorite save lost the imported endpoint/label")
    except (OSError, TimeoutError, ValueError, RuntimeError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        report["errors"].append(str(error))
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        fixture.close()
        log = home / "main/khz_client.log"
        if log.is_file():
            (output / "native-client.log").write_bytes(log.read_bytes())
        report["elapsed_seconds"] = time.monotonic() - started
        report["passed"] = "cache" in report and not report["errors"]
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report.get(key) for key in ("passed", "elapsed_seconds", "errors")}, indent=2))
        print(output / "result.json")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
