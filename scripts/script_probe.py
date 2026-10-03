"""Run the embedded GSC probe against an isolated test server."""
import argparse
import json
from pathlib import Path
import socket
import time

parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=28961)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
deadline = time.monotonic() + 30
reply = ''
while time.monotonic() < deadline:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(.5)
        sock.sendto(b'\xff' * 4 + b'rcon local-khz-benchmark khz_script_probe', ('127.0.0.1', args.port))
        try:
            reply = sock.recv(65536).decode('latin1')
        except OSError:
            pass
    if 'passed^7' in reply or 'failed ' in reply:
        break
    time.sleep(1.1)
report = {'passed': 'passed^7' in reply, 'engine_reply': reply,
          'checks': ['zero wait yields one tick', '1 ms wait', '50 ms wait',
                     '100 ms wait', 'positive wait rounds upward',
                     'current frame end', 'repeated frame end yields',
                     'listener rearms for four notifications in one frame-end phase',
                     'deadline clock starts at 0xFFFFF0 and crosses former wrap']}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2), encoding='utf8')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['passed'] else 1)
