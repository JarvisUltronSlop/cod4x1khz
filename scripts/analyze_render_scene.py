"""Summarize retained camera-verified scenes and time-aligned OS usage samples."""
import argparse
from datetime import datetime, timedelta
import hashlib
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    args = parser.parse_args(); directory = args.fixture.resolve()
    result_path, usage_path = directory / 'result.json', directory / 'usage.json'
    result, usage = json.loads(result_path.read_text()), json.loads(usage_path.read_text())
    if not result['passed'] or usage['errors'] or not result['cleanup']['exact_owned_server_exited'] or result['cleanup']['created_client_returncode'] != 0:
        raise ValueError('Native camera fixture, usage observer and normal owned cleanup must pass')
    client, server = str(usage['client_pid']), str(usage['server_pid'])
    if usage['client_pid'] != result['owned_client_pid'] or usage['server_pid'] != result['owned_server']['pid']:
        raise ValueError('Usage observer does not belong to the exact native game processes')
    stages = []
    for stage in result['stages']:
        start, finish = map(datetime.fromisoformat, (stage['measurement_started_utc'], stage['measurement_finished_utc']))
        # Counters describe the preceding sample interval. Exclude the first
        # boundary-crossing interval rather than include warmup as gameplay.
        rows = [r for r in usage['samples'] if start + timedelta(seconds=1) <= datetime.fromisoformat(r['recorded_utc']) <= finish]
        if len(rows) < 2:
            raise ValueError('Insufficient time-aligned usage samples')
        busy = [r['gpu']['processes'][client]['busiest_engine_percent'] for r in rows if r['gpu'].get('available') and r['gpu']['processes'][client]['busiest_engine_percent'] is not None]
        copy = [v for r in rows if r['gpu'].get('available') for k, v in r['gpu']['processes'][client]['engines'].items() if 'Copy' in k]
        elapsed = rows[-1]['elapsed'] - rows[0]['elapsed']
        cores = {label: (rows[-1]['processes'][pid]['cpu_seconds'] - rows[0]['processes'][pid]['cpu_seconds']) / elapsed for label, pid in (('client', client), ('server', server))}
        stages.append({'scene': stage['scene'], 'cap': stage['cap'], 'passed': stage['passed'],
            'present_fps': stage['client']['gpu_presents_hz'], 'fps_intervals': stage['fps_intervals'],
            'server_hz': stage['server']['ticks_hz'], 'client_hz': {n: stage['client'][n] for n in ('prediction_ticks_hz', 'commands_sent_hz', 'snapshots_received_hz')},
            'accepted_input_hz': stage['admission']['accepted_ack_input_hz'], 'bot_slot_hz_range': [min(v['command_ticks_hz'] for v in stage['bot_controls']['slots'].values()), max(v['command_ticks_hz'] for v in stage['bot_controls']['slots'].values())],
            'server_work_p99_us': stage['work_p99_us'], 'server_end_debt': stage['end_debt'], 'client_debt': stage['client_debt'],
            'gpu_busy_percent': {'samples': len(busy), 'mean': statistics.mean(busy) if busy else None, 'maximum': max(busy) if busy else None},
            'copy_engine_maximum_percent': max(copy) if copy else None, 'cpu_core_equivalents': cores,
            'client_peak_private_bytes': max(r['processes'][client]['private_bytes'] for r in rows)})
    report = {'map': result['map'], 'resolution': result['resolution'], 'timing_seconds': result.get('timing_seconds', 0),
        'source_result_sha256': hashlib.sha256(result_path.read_bytes()).hexdigest(), 'usage_sha256': hashlib.sha256(usage_path.read_bytes()).hexdigest(),
        'analysis_script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'stages': stages,
        'scope': 'Actual verified scenes with fifteen combat bots and a protected diagnostic human. CPU cores are process CPU time divided by elapsed time. WDDM busiest-engine busy time is not shader throughput. FPS distribution uses approximately one-second intervals, not individual-frame 1% lows. These views are not a whole-map normal-combat average or deterministic replay.'}
    (directory / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'output': str(directory / 'analysis.json'), 'stages': len(stages)}))


if __name__ == '__main__': main()
