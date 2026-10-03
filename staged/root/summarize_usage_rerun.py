"""Summarize actual benchmark and timestamp-aligned read-only CPU counters."""
from pathlib import Path
from datetime import datetime
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[2]
REPORT = ROOT / 'results/mp_killhouse-60s-uncapped-480p-20261003-124428.json'
PREFIX = ROOT / 'results/usage-20261003174417'
load = lambda path: json.loads(path.read_text(encoding='utf-8-sig'))
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report = load(REPORT)
summary = report['summary']
cpu_path = Path(str(PREFIX) + '-cpu.json')
console_path = Path(str(PREFIX) + '-console.jsonl')
adapters_path = ROOT / 'results/usage-gpu-adapters-20261003.json'
lines = [json.loads(line) for line in console_path.read_text().splitlines()]
markers = {match[1]: datetime.fromisoformat(line['recorded_utc'])
           for line in lines if (match := re.match(r'^(0\.0|60\.0)s: server p99 ', line['line']))}
start, end = markers['0.0'], markers['60.0']
cpu_samples = [sample for sample in load(cpu_path)['samples']
               if start <= datetime.fromisoformat(sample['recorded_utc']) <= end]
if len(cpu_samples) < 25:
    raise ValueError('Too few timestamp-aligned logical processor samples')
cpu = {}
for instance in cpu_samples[0]['processor_busy_percent']:
    values = [sample['processor_busy_percent'][instance] for sample in cpu_samples]
    if any(value is None for value in values):
        raise ValueError('Unavailable CPU counter: ' + instance)
    cpu[instance] = {'average_busy_percent': sum(values) / len(values), 'peak_busy_percent': max(values)}

pid = str(summary['rendered_process_id'])
gpu_engines = {}
samples = report['samples']
for index in range(1, len(samples)):
    sample, previous = samples[index], samples[index - 1]
    if not sample['gpu']['available']:
        raise ValueError('GPU counter unavailable during match')
    interval = sample['elapsed'] - previous['elapsed']
    for name, value in sample['gpu']['processes'][pid]['engines'].items():
        engine = gpu_engines.setdefault(name, {'weighted_percent_seconds': 0, 'seconds': 0, 'peak_busy_percent': 0})
        engine['weighted_percent_seconds'] += value * interval
        engine['seconds'] += interval
        engine['peak_busy_percent'] = max(engine['peak_busy_percent'], value)
adapters = {adapter['luid']: adapter for adapter in load(adapters_path)}
for name, engine in gpu_engines.items():
    match = re.match(r'luid_0x([0-9a-fA-F]+)_0x([0-9a-fA-F]+)_phys_', name)
    if not match:
        raise ValueError('Unrecognized GPU adapter instance: ' + name)
    luid = (int(match[1], 16) << 32) | int(match[2], 16)
    engine['adapter'] = adapters[luid]['description']
    engine['average_busy_percent'] = engine.pop('weighted_percent_seconds') / engine['seconds']

flags = ('rates_pass', 'p99_pass', 'debt_pass', 'gameplay_pass', 'independent_owner_pass',
         'bots_pass', 'bot_controls_pass', 'collision_pass', 'script_pass', 'render_pass',
         'render_cap_pass', 'complete', 'provenance_pass')
if not all(summary.get(flag) is True for flag in flags):
    raise ValueError('A native timing/gameplay/provenance acceptance flag failed')
total_cores = sum(process['average_cpu_cores'] for process in summary['processes'].values())
output = {
    'schema': 1, 'build': report['provenance']['before_launch']['source_archive']['build'],
    'timing_case_passed': True, 'release_gate_passed': False,
    'measurement_markers_utc': {'start': start.isoformat(), 'end': end.isoformat()},
    'configuration': {'map': report['map'], 'seconds': 60, 'humans': 1, 'bots': 15,
                      'resolution': report['resolution'], 'render_cap': report['render_cap'],
                      'vsync': 'off in hashed benchmark configuration'},
    'summary': summary,
    'native_server_work_p99_us': samples[-1]['server']['work_p99_us'],
    'cpu': {'logical_processors': report['logical_cpus'],
            'game_cpu_core_equivalents': total_cores,
            'game_share_of_total_logical_cpu_percent': total_cores / report['logical_cpus'] * 100,
            'system_aligned_sample_count': len(cpu_samples), 'logical_processor_busy': cpu},
    'gpu_engines': gpu_engines,
    'inputs': {str(path.relative_to(ROOT)): sha(path) for path in
               (REPORT, cpu_path, console_path, adapters_path,
                ROOT / 'scripts/cpu_usage_capture.ps1', ROOT / 'staged/root/usage_rerun.ps1')},
    'cleanup': report['provenance']['cleanup'],
    'interpretation': 'Measured hardware headroom and rendering below 1000 FPS. Frontend, backend and Present timings overlap and include waits; these aggregate counters do not identify a function-level bottleneck. Authoritative simulation, client prediction, human transport and all fifteen bot control rates pass 1000 Hz tolerance in this case.',
    'limitations': 'System CPU counters include other applications and use the sample arithmetic mean between console markers. GPU values are WDDM engine busy time, not shader throughput. One Killhouse run does not establish full gameplay correctness, current all-map acceptance, public-feature completeness or renderer-active full-world replay.'
}
path = Path(str(PREFIX) + '-summary.json')
path.write_text(json.dumps(output, indent=2) + '\n')
print(json.dumps({'output': str(path), 'timing_case_passed': True,
                  'fps': summary['render_present_fps'],
                  'game_cpu_percent': output['cpu']['game_share_of_total_logical_cpu_percent'],
                  'system_cpu_percent': cpu['_total']['average_busy_percent'],
                  'gpu_engines': {name: engine['average_busy_percent'] for name, engine in gpu_engines.items()}}))
