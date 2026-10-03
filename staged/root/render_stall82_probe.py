"""Read-only bounded stall diagnosis; samples containing inspection are invalid for timing."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from monitor import query, ProcessSampler
from gpu_metrics import GpuSampler

p = argparse.ArgumentParser()
p.add_argument('--client', type=int, required=True)
p.add_argument('--server', type=int, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
out = a.output.resolve()
if out.exists() or not out.is_relative_to(ROOT / 'results'):
    raise ValueError('Fresh confined result directory required')
out.mkdir()
inspector = ROOT / 'build/Release/Release/khz_inspect.exe'
report = {'profiling_only': True, 'timing_valid': False, 'client_pid': a.client,
          'server_pid': a.server, 'samples': [], 'errors': [], 'stacks': [],
          'helper_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'inspector_sha256': hashlib.sha256(inspector.read_bytes()).hexdigest()}
cpu, gpu = ProcessSampler([a.client, a.server]), GpuSampler([a.client, a.server])
try:
    start = time.monotonic()
    for i in range(31):
        time.sleep(max(0, start + i - time.monotonic()))
        sample = {'elapsed': time.monotonic() - start, 'processes': cpu.sample(), 'gpu': gpu.sample()}
        try:
            sample['server'] = query(28960, 'rcon local-khz-benchmark khz_json')
            sample['client'] = query(28962, 'khz_stats')
        except (OSError, ValueError) as e:
            report['errors'].append(str(e))
            for n in range(2):
                if n: time.sleep(3)
                file = out / f'stacks-{n}.txt'
                with file.open('wb') as f:
                    r = subprocess.run([str(inspector), str(a.client), str(ROOT / 'engine/bin/Release')], stdout=f, stderr=subprocess.STDOUT, timeout=10)
                report['stacks'].append({'path': str(file), 'returncode': r.returncode})
            report['samples'].append(sample)
            break
        report['samples'].append(sample)
finally:
    cpu.close(); gpu.close()
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'samples': len(report['samples']), 'errors': report['errors'], 'stacks': report['stacks']}))
