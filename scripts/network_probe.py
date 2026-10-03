"""Observe the real client through impairment and a clean recovery period."""
import argparse
import json
from pathlib import Path
import time
from monitor import query

parser = argparse.ArgumentParser()
parser.add_argument('--proxy-output', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
samples = []
start = time.monotonic()
errors = []
while time.monotonic() - start < 25:
    try:
        samples.append({'elapsed': time.monotonic() - start,
                        'client': query(28972, 'khz_stats'),
                        'state': query(28972, 'khz_state', 'KHZ_STATE'),
                        'server': query(28971, 'rcon local-khz-benchmark khz_json')})
    except (OSError, ValueError) as exc:
        errors.append(str(exc))
    time.sleep(2)
until = time.monotonic() + 10
while not args.proxy_output.exists() and time.monotonic() < until:
    time.sleep(.5)
proxy = json.loads(args.proxy_output.read_text(encoding='utf8')) if args.proxy_output.exists() else {}
clean = [s for s in samples if s['elapsed'] >= 17]
rates = {}
if len(clean) >= 2:
    first, last = clean[0]['client'], clean[-1]['client']
    seconds = last['seconds'] - first['seconds']
    rates = {name: (last[name] - first[name]) / seconds for name in ('prediction_ticks', 'commands_sent', 'snapshots_received')}
passed = (bool(rates) and all(999 <= hz <= 1001 for hz in rates.values())
          and all(s['state']['connection'] == 9 for s in samples)
          and not errors and proxy.get('lost', 0) > 0 and proxy.get('duplicates', 0) > 0
          and proxy.get('reordered', 0) > 0 and proxy.get('overflow', 1) == 0
          and proxy.get('peak_packets', 8193) <= 8192
          and proxy.get('peak_bytes', 8388609) <= 8388608)
report = {'passed': passed, 'recovery_rates': rates, 'proxy': proxy, 'errors': errors,
          'samples': samples, 'scope': 'loss/jitter/duplication/reordering and clean rate recovery; disconnect, map change and rewind tests remain separate'}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2), encoding='utf8')
print(json.dumps({'passed': passed, 'recovery_rates': rates, 'proxy': proxy, 'errors': errors}, indent=2))
raise SystemExit(0 if passed else 1)
