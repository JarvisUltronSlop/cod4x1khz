"""Validate actual archived timing reports; no live queries or source changes."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
LOAD = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
FLAGS = ('rates_pass', 'p99_pass', 'debt_pass', 'gameplay_pass', 'independent_owner_pass',
         'bots_pass', 'bot_controls_pass', 'collision_pass', 'script_pass', 'render_pass',
         'render_cap_pass', 'complete', 'provenance_pass')

def main():
    source = ROOT / 'results/build74-timing-cases.json'
    cases = LOAD(source)
    archive = ROOT / 'results/build74-source-and-binary-hashes.json'
    record = LOAD(archive)
    checked = []
    for case in cases['cases']:
        errors = []
        if not case['wrapper_pass'] or len(case['reports']) != 1:
            checked.append({**case, 'passed': False, 'errors': ['Benchmark wrapper failed or report count differs']})
            continue
        path = ROOT / case['reports'][0]
        report = LOAD(path)
        summary = report['summary']
        errors += ['Missing/failed acceptance: ' + key for key in FLAGS if summary.get(key) is not True]
        if report.get('requested_seconds') != 60 or report.get('resolution') != '480p' or report.get('render_cap') != 0 or report.get('map') != case['map']:
            errors.append('Requested actual map/duration/cap/resolution differs')
        binding = report['provenance']['before_launch']['source_archive']
        if binding.get('build') != 74 or binding.get('sha256') != hashlib.sha256(archive.read_bytes()).hexdigest():
            errors.append('Actual build provenance does not bind immutable74 archive')
        samples = [sample['server']['server_phases']['archive'] for sample in report['samples']]
        first, last = samples[0], samples[-1]
        duration = (last['native_time'] - first['native_time']) / 1000
        commits = last['committed_frames'] - first['committed_frames']
        attempts = last['attempts'] - first['attempts']
        overflows = last['overflows'] - first['overflows']
        skipped = last['skipped_opportunities'] - first['skipped_opportunities']
        archive_hz = commits / duration if duration > 0 else 0
        if not 59.9 <= duration <= 60.1 or abs(archive_hz - 20) > .02 or attempts != commits or overflows or skipped:
            errors.append('Native archive cadence/attempt/overflow/skip interval fails')
        for sample in samples:
            if sample.get('sample_msec') != 50 or sample.get('epoch') != first['epoch'] or sample.get('attempt_active') is not False or sample.get('index_valid') is not True:
                errors.append('Sampled archive generation/index/attempt boundary invalid')
                break
        phase = report['samples'][-1]['server']['server_phases']['phases']['archive']
        detail = {'schema': 1, 'build': 74, 'report': case['reports'][0], 'report_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                  'build_archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(), 'passed': not errors,
                  'errors': errors, 'native_duration_seconds': duration, 'committed_frame_delta': commits,
                  'attempt_delta': attempts, 'overflows_delta': overflows, 'skipped_delta': skipped,
                  'archive_hz': archive_hz, 'first': first, 'last': last, 'archive_phase': phase,
                  'summary': summary, 'scope': 'Actual timing and sampled native archive policy diagnostics; native codec oracle and full-world renderer replay remain separate.'}
        output = path.with_name(path.stem + '-archive-validation.json')
        output.write_text(json.dumps(detail, indent=2) + '\n')
        checked.append({**case, 'passed': not errors, 'errors': errors, 'archive_validation': str(output.relative_to(ROOT)),
                        'server_work_p99_us': report['samples'][-1]['server']['work_p99_us'],
                        'render_present_fps': summary['render_present_fps'], 'archive_hz': archive_hz})
    result = {'schema': 1, 'build': 74, 'matrix_pass': len(checked) == 4 and all(case['passed'] for case in checked),
              'cases': checked, 'release_gate_passed': False,
              'scope': 'Current-build timing only; complete public features, native codec oracle, full physics/bot behavior and ALL14 renderer-active replay remain unpassed.'}
    output = ROOT / 'results/build74-archive-matrix-validation.json'
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'matrix_pass': result['matrix_pass'], 'result': str(output)}))
    return 0 if result['matrix_pass'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
