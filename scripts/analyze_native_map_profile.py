"""Analyze the exact checked interval of a retained native map fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
from vulkan_frame_profile import analyze


def analyze_fixture(path):
    raw = path.read_bytes()
    data = json.loads(raw)
    if data['client_exit'] or data['server_exit'] or not data['measurement']['camera_passed']:
        raise ValueError('Owned processes and checked broad-view interval must pass')
    samples = [sample for sample in data['samples'] if
        sample['client']['seconds'] >= data['broad_view']['after_seconds'] + 3 and
        sample['server'].get('native_bots', {}).get('active') == 2 and
        all(not data.get(key) or sample['client']['seconds'] < data[key]['after_seconds']
            for key in ('profile_export', 'cpu_profile', 'cpu_sampling'))]
    if len(samples) != data['measurement']['samples']:
        raise ValueError('Reconstructed interval differs from the fixture measurement')
    values = [max(process['busiest_engine_percent'] for process in sample['gpu']['processes'].values())
        for sample in samples if sample['gpu']['available']]
    report = {'source': str(path), 'source_sha256': hashlib.sha256(raw).hexdigest(),
              'binary_sha256': data['binary_sha256'], 'measurement': data['measurement'],
              'gpu_utilization': {'scope': 'Owned-process WDDM busiest-engine busy time, not peak shader throughput',
                  'samples': len(values), 'mean_percent': statistics.mean(values) if values else None},
              'full_performance_acceptance_verified': False}
    if data['renderer'] == 'vulkan' and data.get('profile_export'):
        trace = next(Path(p) for p in data['diagnostic_paths'] if p.endswith('frame-timings.csv'))
        detail = analyze(trace, samples[0]['client']['native_graphics']['trace_clock_us'],
                         samples[-1]['client']['native_graphics']['trace_clock_us'])
        if not detail['timing_capture_complete']:
            raise ValueError('Native Vulkan timings are incomplete in the exact measured interval')
        report['native_timings'] = detail
    first, last = samples[0], samples[-1]
    before, after = first['client'].get('native_graphics', {}), last['client'].get('native_graphics', {})
    if all(key in before and key in after for key in ('execute_us', 'lease_retire_us', 'query_poll_us', 'queue_wait_us')):
        frames = after['frames'] - before['frames']
        if frames <= 0:
            raise ValueError('No completed native frames in the checked interval')
        report['submission_owner'] = {
            'scope': 'Cumulative CPU owner counters divided by completed native presentation count; boundary work can differ by one frame',
            'completed_frames': frames,
            'mean_us': {key: (after[key] - before[key]) / frames for key in
                        ('execute_us', 'lease_retire_us', 'query_poll_us', 'queue_wait_us')}}
    prediction = last['client']['prediction_ticks'] - first['client']['prediction_ticks']
    report['prediction_steps_per_tick'] = (last['client']['prediction_steps'] - first['client']['prediction_steps']) / prediction
    report['server_admission_rejections'] = last['server']['time_rejected'] - first['server']['time_rejected']
    path.with_suffix('.native-analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    analyze_fixture(parser.parse_args().fixture)
