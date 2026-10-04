"""Analyze real CPU spans and D3D9 timestamps; never sum overlapping stages."""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path

def distribution(values):
    values = sorted(values)
    return {'count': len(values), 'average_us': sum(values) / len(values),
            'p50_us': values[math.ceil(len(values) * .50) - 1],
            'p99_us': values[math.ceil(len(values) * .99) - 1], 'max_us': values[-1]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    fixture = args.fixture.resolve()
    if fixture.is_file():
        result = json.loads(fixture.read_text())
        if result['client_exit'] or result['server_exit'] or not result['measurement']['camera_passed'] or result.get('cpu_profile_error'):
            raise ValueError('Packaged profile camera and exact process cleanup must pass')
        capture = Path(result['cpu_profile_path'])
        capture_sha256 = result['cpu_profile']['capture_sha256']
        analysis_file = fixture.with_suffix('.profile-analysis.json')
        timeline_file = fixture.with_suffix('.profile-timeline.json')
    else:
        result = json.loads((fixture / 'result.json').read_text())
        if not result['passed'] or result['cleanup']['created_client_returncode'] != 0 or not result['cleanup']['exact_owned_server_exited']:
            raise ValueError('Actual profile fixture and exact cleanup must pass')
        capture = fixture / 'fixture-client-home' / result['capture_file']['relative_to_home']
        capture_sha256 = result['capture_file']['sha256']
        analysis_file = fixture / 'analysis.json'
        timeline_file = fixture / 'timeline.json'
    if hashlib.sha256(capture.read_bytes()).hexdigest() != capture_sha256:
        raise ValueError('Native frozen capture differs from recorded hash')
    data = json.loads(capture.read_text())
    status = data['status']
    if not status['gpu_supported'] or status['disjoint'] or not status['gpu_frequency'] or status['query_errors'] or status['cpu_overflow'] or status['gpu_skipped']:
        raise ValueError('GPU timestamps or CPU spans cannot establish a complete capture')
    cpu_scale, gpu_scale = 1e6 / status['qpc_frequency'], 1e6 / status['gpu_frequency']
    duration_us = (status['end_qpc'] - status['start_qpc']) * cpu_scale
    cpu = defaultdict(list)
    for event in data['cpu']:
        if event['end'] < event['begin'] or event['role'] not in (0, 1):
            raise ValueError('Invalid native CPU event')
        cpu[(event['role'], event['stage'])].append((event['end'] - event['begin']) * cpu_scale)
    aggregates = {}
    for value in data.get('cpu_aggregates', []):
        if value['role'] not in (0, 1) or value['count'] <= 0 or value['ticks'] < value['max_ticks'] or value['max_ticks'] < 0:
            raise ValueError('Invalid native CPU aggregate')
        aggregates[f"{value['role']}:{value['stage']}"] = {
            'count': value['count'], 'average_us': value['ticks'] * cpu_scale / value['count'],
            'total_us': value['ticks'] * cpu_scale, 'max_us': value['max_ticks'] * cpu_scale,
            'capture_wall_fraction': value['ticks'] * cpu_scale / duration_us}
    gpu = data['gpu']
    present_ends = sorted(event['end'] for event in data['cpu'] if event['role'] == 1 and event['stage'] == 'present')
    present_intervals = [(b - a) * cpu_scale for a, b in zip(present_ends, present_ends[1:])]
    if not present_intervals or min(present_intervals) <= 0:
        raise ValueError('Native presentation completion intervals are missing or unordered')
    if len(gpu) < 1000 or any(frame['end'] < frame['begin'] or frame['first_ready'] < frame['submit_end'] or not frame['first_ready'] for frame in gpu):
        raise ValueError('GPU frame duration or CPU issue/completion bracket invalid')
    if any(right['begin'] < left['end'] for left, right in zip(gpu, gpu[1:])):
        raise ValueError('GPU timestamp intervals overlap or run backward')
    work = [(frame['end'] - frame['begin']) * gpu_scale for frame in gpu]
    gaps = [(right['begin'] - left['end']) * gpu_scale for left, right in zip(gpu, gpu[1:])]
    gpu_span = (gpu[-1]['end'] - gpu[0]['begin']) * gpu_scale
    submit = [(frame['submit_end'] - frame['submit_begin']) * cpu_scale for frame in gpu]
    ready = [(frame['first_ready'] - frame['submit_end']) * cpu_scale for frame in gpu]
    anchor = gpu[0]['begin']
    lower = max((frame['submit_begin'] - status['start_qpc']) * cpu_scale - (frame['begin'] - anchor) * gpu_scale for frame in gpu)
    upper = min((frame['first_ready'] - status['start_qpc']) * cpu_scale - (frame['end'] - anchor) * gpu_scale for frame in gpu)
    if upper < lower:
        raise ValueError('CPU/GPU clock alignment brackets have no intersection')
    summary = {'schema': 1, 'fixture': str(fixture.relative_to(root)), 'capture_sha256': capture_sha256,
               'duration_seconds': duration_us / 1e6, 'profiled_frame_rate': len(gpu) / (duration_us / 1e6),
               'gpu_work': distribution(work), 'gpu_inter_frame_gap': distribution(gaps),
               'gpu_measured_command_interval_fraction': sum(work) / gpu_span,
               'cpu_submission': distribution(submit), 'completion_observation_delay': distribution(ready),
               'present_completion_intervals': distribution(present_intervals),
               'fps_from_p99_present_interval': 1e6 / distribution(present_intervals)['p99_us'],
               'present_interval_scope': 'CPU timestamps between actual Present returns during profiling; not display photon timings or an unprofiled throughput gate.',
               'clock_alignment_offset_us': {'lower': lower, 'upper': upper, 'uncertainty': upper - lower},
               'cpu_stages': {f'{role}:{stage}': {**distribution(values), 'capture_wall_fraction': sum(values) / duration_us} for (role, stage), values in cpu.items()},
               'cpu_aggregates': aggregates,
               'aggregate_scope': 'Inclusive call totals; nested calls overlap. No per-call timeline or percentiles are stored.',
               'scope': 'Real native GPU timestamps and CPU wait spans; asynchronous zero-flush queries add overhead. Completion is first observation, not exact GPU completion time. GPU command intervals can contain internal bubbles and exclude Present. Clock alignment is bounded, not assumed exact. No performance gate or full-world replay claim.'}
    analysis_file.write_text(json.dumps(summary, indent=2) + '\n')
    # Chrome/Perfetto trace with actual CPU intervals and midpoint GPU alignment;
    # report the uncertainty explicitly rather than presenting exact synchronization.
    offset = (lower + upper) / 2
    events = [{'name': event['stage'], 'ph': 'X', 'pid': 1, 'tid': event['thread'],
               'ts': (event['begin'] - status['start_qpc']) * cpu_scale,
               'dur': (event['end'] - event['begin']) * cpu_scale} for event in data['cpu']]
    events += [{'name': 'GPU frame commands', 'ph': 'X', 'pid': 2, 'tid': 0,
                'ts': offset + (frame['begin'] - anchor) * gpu_scale,
                'dur': (frame['end'] - frame['begin']) * gpu_scale} for frame in gpu]
    timeline_file.write_text(json.dumps({'traceEvents': events, 'displayTimeUnit': 'ms',
        'clockAlignmentUncertaintyMicroseconds': upper - lower}, separators=(',', ':')))
    print(json.dumps(summary, indent=2))

if __name__ == '__main__':
    main()
