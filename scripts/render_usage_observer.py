"""Bounded read-only actual process CPU/memory and WDDM GPU engine observer."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import time
from gpu_metrics import GpuSampler
from monitor import ProcessSampler

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--client', required=True, type=int)
    p.add_argument('--server', required=True, type=int)
    p.add_argument('--seconds', type=int, default=65)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if not 1 <= a.seconds <= 120: raise ValueError('Observer duration exceeds bound')
    gpu, cpu = GpuSampler([a.client, a.server]), ProcessSampler([a.client, a.server])
    report = {'client_pid': a.client, 'server_pid': a.server, 'samples': [], 'errors': [],
        'scope': 'Read-only OS process times/memory and WDDM busiest-engine busy time; GPU engines are not summed or equated to shader throughput.'}
    try:
        start = time.monotonic()
        for index in range(a.seconds):
            delay = start + index + 1 - time.monotonic()
            if delay > 0: time.sleep(delay)
            try:
                report['samples'].append({'recorded_utc': datetime.now(timezone.utc).isoformat(), 'elapsed': time.monotonic() - start,
                    'gpu': gpu.sample(), 'processes': cpu.sample()})
            except OSError as e:
                report['errors'].append(str(e)); break # The exact owned game has exited.
    finally:
        gpu.close(); cpu.close()
        a.output.write_text(json.dumps(report, indent=2) + '\n')

if __name__ == '__main__': main()
