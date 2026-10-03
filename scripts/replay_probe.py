import argparse
import json
from pathlib import Path
import socket
from monitor import query

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
report = query(28961, 'rcon local-khz-benchmark khz_replay', 'KHZ_REPLAY', timeout=30)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2), encoding='utf8')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['passed'] else 1)
