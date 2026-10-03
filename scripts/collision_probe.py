"""Read-only collision-frame shadow parity on one active rendered client."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time
from monitor import query


REPORT_NAME = 'khz_collision_shadow.json'
ZERO_COUNTS = ('build_issues', 'trace_mismatches', 'ordering_ambiguities',
               'pose_mismatches', 'coverage_mismatches')
GUARD_FIELDS = ('guards_run', 'guards_pass', 'guards_required_mask', 'guards_passed_mask',
                'guards_failed_mask', 'guards_skipped_mask')
MATCH_FIELDS = ('completed', 'passed', 'frame_complete', 'map_checksum', 'sample_time',
                'current_snapshot_time', 'next_snapshot_time', 'entities',
                'dynamic_brushes', 'dynamic_source', 'sweeps', *ZERO_COUNTS, *GUARD_FIELDS)


def validate(summary, full):
    errors = []
    for field in ('completed', 'passed', 'frame_complete', 'report_written', 'guards_run', 'guards_pass'):
        if summary.get(field) is not True:
            errors.append(f'Endpoint {field} is not true')
    if summary.get('report') != REPORT_NAME:
        errors.append('Endpoint reported an unexpected collision report filename')
    sweeps = summary.get('sweeps')
    if type(sweeps) is not int or sweeps <= 0:
        errors.append('No completed shadow collision sweeps')
    for field in ZERO_COUNTS:
        if type(summary.get(field)) is not int or summary[field] != 0:
            errors.append(f'Endpoint {field} is missing or nonzero')
    for field, required in (('guards_required_mask', 511), ('guards_passed_mask', 511),
                            ('guards_failed_mask', 0), ('guards_skipped_mask', 0)):
        if type(summary.get(field)) is not int or summary[field] != required:
            errors.append(f'Endpoint {field} must equal {required}')
    for field in MATCH_FIELDS:
        if field not in summary or field not in full or summary[field] != full[field]:
            errors.append(f'Full report differs from endpoint field {field}')
    # The engine bounds detailed samples independently of mismatch counters.
    for field in ('issues', 'failures', 'poses', 'coverage'):
        if not isinstance(full.get(field), list):
            errors.append(f'Full report is missing the bounded {field} array')
    if full.get('issues') or full.get('failures'):
        errors.append('Full report contains build issues or parity failures')
    return errors


def signature(path):
    if not path.exists():
        return None
    stat = path.stat()
    return stat.st_mtime_ns, stat.st_size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--client-port', type=int, default=28982)
    parser.add_argument('--map', choices=('mp_killhouse', 'mp_crash'), default='mp_killhouse')
    parser.add_argument('--headless-clients', type=int, choices=range(4), default=0)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1024 <= args.client_port <= 65535:
        parser.error('--client-port must be between 1024 and 65535')
    root = Path(__file__).resolve().parent.parent
    if not args.output.resolve().is_relative_to(root):
        parser.error('--output must remain beneath the project directory')
    source_report = root / 'runtime' / REPORT_NAME
    saved_full = args.output.with_name(args.output.stem + '-full.json')
    report = {
        'map': args.map, 'client_port': args.client_port, 'resolution': '720p', 'render_cap': 0,
        'headless_clients': args.headless_clients,
        'started_utc': datetime.now(timezone.utc).isoformat(), 'errors': [],
        'scope': 'Short read-only shadow diagnostic of one captured client collision frame '
                 'against legacy traces. The diagnostic preserves active production state '
                 'and restores its prior collision-frame binding. '
                 'This does not establish live full-world collision or Pmove acceptance.'}
    started = time.monotonic()
    try:
        report['binary_sha256'] = {
            name: hashlib.sha256((root / 'runtime' / name).read_bytes()).hexdigest()
            for name in ('cod4x1khz.exe', 'cod4x1khz_server.exe')}
        report['source_pins'] = json.loads((root / 'upstream-lock.json').read_text(encoding='utf8'))
        report['before_state'] = query(args.client_port, 'khz_state', 'KHZ_STATE')
        state = report['before_state']
        if not isinstance(state, dict):
            raise ValueError('Client state reply is not a JSON object')
        if (state.get('connection') != 9 or state.get('pm_type') != 0
                or state.get('other_flags', 0) & 2 or not state.get('weapon')):
            raise RuntimeError('Collision shadow probe requires an independent spawned player')
        before_signature = signature(source_report)
        summary = query(args.client_port, 'khz_collision', 'KHZ_COLLISION', timeout=20)
        report['summary'] = summary
        if not isinstance(summary, dict):
            raise ValueError('Collision shadow summary is not a JSON object')
        if summary.get('report') != REPORT_NAME or not summary.get('report_written'):
            raise RuntimeError('Engine did not write the expected collision shadow report')
        after_signature = signature(source_report)
        if after_signature is None or after_signature == before_signature:
            raise RuntimeError('Collision shadow full report is missing or stale')
        full_bytes = source_report.read_bytes()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        saved_full.write_bytes(full_bytes)
        report['full_report'] = str(saved_full.resolve())
        report['full_report_sha256'] = hashlib.sha256(full_bytes).hexdigest()
        full = json.loads(full_bytes.decode('utf8'))
        if not isinstance(full, dict):
            raise ValueError('Collision shadow full report is not a JSON object')
        report['errors'].extend(validate(summary, full))
        report['after_state'] = query(args.client_port, 'khz_state', 'KHZ_STATE')
        if (report['after_state'].get('connection') != 9
                or report['after_state'].get('server_id') != state.get('server_id')):
            report['errors'].append('Client disconnected or changed session during the shadow probe')
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as exc:
        report['errors'].append(str(exc))
    finally:
        report['elapsed_seconds'] = time.monotonic() - started
        report['passed'] = bool(report.get('summary')) and not report['errors']
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
        print(json.dumps({key: report.get(key) for key in
                          ('passed', 'summary', 'full_report', 'elapsed_seconds', 'errors')}, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
