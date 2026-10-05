"""Inspect complete native CPU spans, including Vulkan CPU-only captures."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path


def analyze(fixture):
    report = json.loads(fixture.read_text())
    if (report['client_exit'] or report['server_exit'] or
            not report['measurement']['camera_passed'] or report.get('cpu_profile_error') or
            not report.get('cpu_profile_camera', {}).get('passed')):
        raise ValueError('Capture camera, completeness and exact process cleanup must pass')
    path = Path(report['cpu_profile_path'])
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != report['cpu_profile']['capture_sha256']:
        raise ValueError('Frozen capture differs from fixture hash')
    data = json.loads(raw)
    status = data['status']
    if status['phase'] != 4 or status['cpu_overflow'] or status['query_errors']:
        raise ValueError('CPU capture is incomplete')
    if len(data['cpu']) != status['cpu_main'] + status['cpu_backend']:
        raise ValueError('CPU event count differs from frozen counters')
    scale = 1e6 / status['qpc_frequency']
    duration_us = (status['end_qpc'] - status['start_qpc']) * scale
    if duration_us <= 0:
        raise ValueError('Invalid capture duration')
    stages = defaultdict(list)
    for event in data['cpu']:
        if event['end'] < event['begin'] or event['role'] not in (0, 1):
            raise ValueError('Invalid CPU event')
        stages[f"{event['role']}:{event['stage']}"].append((event['end'] - event['begin']) * scale)
    summary = {'capture_sha256': hashlib.sha256(raw).hexdigest(),
               'duration_seconds': duration_us / 1e6,
               'scope': 'Inclusive CPU stages overlap across and within threads; do not sum them. GPU timings are separate.',
               'stages': {}, 'aggregates': {}}
    frontend = sorted((event['begin'], event['end']) for event in data['cpu']
                      if event['role'] == 0 and event['stage'] == 'frontend')
    backend = sorted((event['begin'], event['end']) for event in data['cpu']
                     if event['role'] == 1 and event['stage'] == 'backend_3d')
    overlap_ticks, backend_index = 0, 0
    for begin, end in frontend:
        while backend_index < len(backend) and backend[backend_index][1] <= begin:
            backend_index += 1
        index = backend_index
        while index < len(backend) and backend[index][0] < end:
            overlap_ticks += max(0, min(end, backend[index][1]) - max(begin, backend[index][0]))
            index += 1
    frontend_ticks = sum(end - begin for begin, end in frontend)
    summary['frontend_backend_overlap'] = {
        'scope': 'Actual intersection of frontend and backend_3d spans on different threads; frontend includes its handoff waits',
        'overlap_seconds': overlap_ticks * scale / 1e6,
        'frontend_fraction': overlap_ticks / frontend_ticks if frontend_ticks else None}
    for name, values in stages.items():
        values.sort()
        summary['stages'][name] = {'count': len(values), 'average_us': sum(values) / len(values),
            'p50_us': values[(len(values) - 1) // 2], 'p99_us': values[(len(values) * 99 - 1) // 100],
            'wall_fraction': sum(values) / duration_us}
    for value in data.get('cpu_aggregates', []):
        if value['role'] not in (0, 1) or value['count'] <= 0 or value['ticks'] < value['max_ticks']:
            raise ValueError('Invalid CPU aggregate')
        summary['aggregates'][f"{value['role']}:{value['stage']}"] = {
            'count': value['count'], 'average_us': value['ticks'] * scale / value['count'],
            'wall_fraction': value['ticks'] * scale / duration_us}
    fixture.with_suffix('.cpu-analysis.json').write_text(json.dumps(summary, indent=2) + '\n')
    events = [{'name': event['stage'], 'ph': 'X', 'pid': 1, 'tid': event['thread'],
               'ts': (event['begin'] - status['start_qpc']) * scale,
               'dur': (event['end'] - event['begin']) * scale} for event in data['cpu']]
    fixture.with_suffix('.cpu-timeline.json').write_text(json.dumps({'traceEvents': events, 'displayTimeUnit': 'ms'}, separators=(',', ':')))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    analyze(parser.parse_args().fixture.resolve())
