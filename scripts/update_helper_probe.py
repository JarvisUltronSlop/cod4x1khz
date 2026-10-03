"""Short isolated signed-update helper regression; fixture images are never executed."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import http.server
import json
from pathlib import Path
import struct
import subprocess
import threading
import time


IDENTITY = b"KHZ_RELEASE_PROJECT=cod4x1khz;protocol=10002;format=1"
INSTALLED = ("cod4x1khz.exe", "runtime/cod4x1khz_server.exe", "runtime/cod4x1khz.exe", "runtime/cod4x1khz_bench.exe")
SOURCES = (0, 1, 0, 0)


def image(variant: int) -> bytes:
    value = bytearray(1024)
    value[:2] = b"MZ"
    struct.pack_into("<I", value, 60, 128)
    value[128:132] = b"PE\0\0"
    struct.pack_into("<HH", value, 132, 0x14C, 1)
    struct.pack_into("<HH", value, 148, 224, 0x102)
    optional, section = 152, 376
    struct.pack_into("<H", value, optional, 0x10B)
    for offset, word in ((16, 4096), (56, 8192), (60, 512)):
        struct.pack_into("<I", value, optional + offset, word)
    struct.pack_into("<H", value, optional + 68, 3)
    value[section:section + 5] = b".text"
    for offset, word in ((8, 512), (12, 4096), (16, 512), (20, 512), (36, 0x60000020)):
        struct.pack_into("<I", value, section + offset, word)
    value[512:514] = bytes((0xC3, variant))
    value[600:600 + len(IDENTITY)] = IDENTITY
    return bytes(value)


class Signer:
    """Actual Windows RSA2048/PKCS1/SHA256 fixture key; public policy contains no private key."""
    def __init__(self):
        self.cng = ctypes.WinDLL("bcrypt")
        self.algorithm, self.key = ctypes.c_void_p(), ctypes.c_void_p()
        self.cng.BCryptOpenAlgorithmProvider.argtypes = [ctypes.POINTER(ctypes.c_void_p), wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.ULONG]
        self.cng.BCryptGenerateKeyPair.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), wintypes.ULONG, wintypes.ULONG]
        self.cng.BCryptFinalizeKeyPair.argtypes = [ctypes.c_void_p, wintypes.ULONG]
        self.cng.BCryptExportKey.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG]
        self.cng.BCryptSignHash.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG]
        self.cng.BCryptDestroyKey.argtypes = [ctypes.c_void_p]
        self.cng.BCryptCloseAlgorithmProvider.argtypes = [ctypes.c_void_p, wintypes.ULONG]
        self.check(self.cng.BCryptOpenAlgorithmProvider(ctypes.byref(self.algorithm), "RSA", None, 0))
        self.check(self.cng.BCryptGenerateKeyPair(self.algorithm, ctypes.byref(self.key), 2048, 0))
        self.check(self.cng.BCryptFinalizeKeyPair(self.key, 0))
        public, length = ctypes.create_string_buffer(283), wintypes.ULONG()
        self.check(self.cng.BCryptExportKey(self.key, None, "RSAPUBLICBLOB", public, 283, ctypes.byref(length), 0))
        if length.value != 283:
            raise RuntimeError("Unexpected RSA2048 public blob length")
        self.public = public.raw

    @staticmethod
    def check(status):
        if ctypes.c_long(status).value < 0:
            raise RuntimeError(f"CNG fixture signing failed: {status & 0xffffffff:08x}")

    def sign(self, body: bytes) -> bytes:
        class Padding(ctypes.Structure):
            _fields_ = [("algorithm", wintypes.LPCWSTR)]
        padding, digest = Padding("SHA256"), ctypes.create_string_buffer(hashlib.sha256(body).digest())
        signature, length = ctypes.create_string_buffer(256), wintypes.ULONG()
        self.check(self.cng.BCryptSignHash(self.key, ctypes.byref(padding), digest, 32, signature, 256, ctypes.byref(length), 2))
        if length.value != 256:
            raise RuntimeError("Unexpected RSA2048 signature length")
        return signature.raw

    def close(self):
        if self.key:
            self.cng.BCryptDestroyKey(self.key)
            self.key = ctypes.c_void_p()
        if self.algorithm:
            self.cng.BCryptCloseAlgorithmProvider(self.algorithm, 0)
            self.algorithm = ctypes.c_void_p()


def hashes(root: Path) -> list[str | None]:
    return [hashlib.sha256((root / path).read_bytes()).hexdigest() if (root / path).exists() else None for path in INSTALLED]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, default=Path("runtime/cod4x1khz_update.exe"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    helper = (project / args.helper).resolve()
    if not helper.is_relative_to(project) or not helper.is_file():
        parser.error("Helper must be a built executable beneath this project")
    output = (args.output or project / "results" / ("update-helper-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S"))).resolve()
    if not output.is_relative_to(project / "results") or output == project / "results" or output.exists():
        parser.error("Output must be a new dedicated directory under project/results")
    root, served = output / "project", output / "http"
    (root / "runtime").mkdir(parents=True)
    (root / "config").mkdir()
    served.mkdir()
    cases = []
    signer = Signer()

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *items, **kwargs):
            super().__init__(*items, directory=str(served), **kwargs)

        def log_message(self, *_items):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    origin = f"http://127.0.0.1:{server.server_port}"
    policy = "KHZ_UPDATE_POLICY_1\nproject=cod4x1khz\norigin=" + origin + "\nmanifest_url=" + origin + "/manifest.txt\ntrusted_key_cng_rsa2048=" + signer.public.hex() + "\nprivate_http_fixture=1\n"
    (root / "config/update-policy.txt").write_text(policy, encoding="ascii", newline="\n")
    for i, path in enumerate(INSTALLED):
        (root / path).write_bytes(image(i))
    old = hashes(root)
    files = (image(10), image(11))
    body = "KHZ_UPDATE_1\nproject=cod4x1khz\nplatform=windows-x86\nprotocol=10002\nversion=1\nclient_mirrors=runtime/cod4x1khz.exe,runtime/cod4x1khz_bench.exe\nfiles=2\n"
    for i, (name, data) in enumerate(zip(INSTALLED[:2], files)):
        route = "client.bin" if i == 0 else "server.bin"
        (served / route).write_bytes(data)
        body += f"file={name}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}\t{origin}/{route}\n"
    signature = signer.sign(body.encode("ascii"))
    (served / "manifest.txt").write_text(body + "signature=" + signature.hex() + "\n", encoding="ascii", newline="\n")
    selected = hashlib.sha256(body.encode("ascii")).hexdigest()
    new = [hashlib.sha256(files[source]).hexdigest() for source in SOURCES]

    def run(name, action, expect, extra=(), fixture=True):
        command = [str(helper), "--action", action, "--fixture-root", str(root)]
        if fixture:
            command += ["--private-http-fixture"]
        command += list(extra)
        started = time.perf_counter()
        process = subprocess.run(command, cwd=output, capture_output=True, text=True, timeout=35, creationflags=subprocess.CREATE_NO_WINDOW)
        (output / (name + ".stdout.log")).write_text(process.stdout, encoding="utf-8")
        (output / (name + ".stderr.log")).write_text(process.stderr, encoding="utf-8")
        result = json.loads(process.stdout) if process.stdout.strip().startswith("{") else {}
        passed = process.returncode == expect
        cases.append({"name": name, "passed": passed, "exit_code": process.returncode, "expected_exit": expect, "elapsed_seconds": time.perf_counter() - started, "result": result})
        if not passed:
            raise AssertionError(name + ": unexpected helper exit")
        return result

    try:
        checked = run("verified-check", "check", 0)
        assert checked["version"] == 1 and checked["manifest_sha256"] == selected
        run("fixture-policy-needs-opt-in", "check", 1, fixture=False)
        run("changed-selected-manifest", "apply", 1, ("--expected-version", "1", "--expected-manifest", "0" * 64))
        assert hashes(root) == old
        apply_args = ("--expected-version", "1", "--expected-manifest", selected)
        run("verified-apply", "apply", 0, apply_args)
        assert hashes(root) == new
        run("healthy-recovery", "recover", 0)
        assert hashes(root) == new
        run("explicit-rollback", "rollback", 0)
        assert hashes(root) == old
        run("reapply", "apply", 0, apply_args)
        journal = root / "updates/transaction.txt"
        journal.write_text(journal.read_text(encoding="ascii").replace("state=committed\n", "state=committing\n", 1), encoding="ascii", newline="\n")
        run("unfinished-journal-recovery", "recover", 0)
        assert hashes(root) == old
        run("fixture-relaunch-refused", "recover", 2, ("--relaunch-image", str(root / INSTALLED[0]), "--relaunch-command", str(root / INSTALLED[0])))
        run("invalid-canonical-version", "apply", 2, ("--expected-version", "01", "--expected-manifest", selected))
        run("apply-before-backup-corruption", "apply", 0, apply_args)
        backup = root / "updates/rollback/1" / INSTALLED[0]
        backup.write_bytes(image(100))
        run("corrupt-backup-refuses-rollback", "rollback", 1)
        assert hashes(root) == new
        journal_bytes = journal.read_bytes()
        journal.write_bytes(journal_bytes.replace(b"signature=", b"signature=x", 1))
        run("corrupt-journal-refuses-recovery", "recover", 1)
        assert hashes(root) == new
    except Exception as error:
        cases.append({"name": "fixture-invariant", "passed": False, "detail": str(error)})
    finally:
        server.shutdown()
        server.server_close()
        thread.join(1)
        signer.close()
    result = {"passed": all(case["passed"] for case in cases), "cases": cases, "binary_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(), "helper": str(helper), "output": str(output), "scope": "Isolated stopped-process helper and signed localhost service fixtures. Fixture PE images are never executed; actual game exit/restart and menu settings persistence remain unverified."}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "cases": len(cases), "output": str(output)}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
