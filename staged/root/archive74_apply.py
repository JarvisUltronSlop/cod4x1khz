"""Apply the root-reviewed source-owned archive probe after exact typechecks.

Does not build or run a game. Retains original bytes and refuses stale inputs.
"""
from datetime import datetime, timezone
from pathlib import Path
import difflib
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / 'staged/archive'
digest = lambda value: hashlib.sha256(value).hexdigest()

def main():
    report_path = STAGE / 'results/20261003-130638-830806/result.json'
    report = json.loads(report_path.read_text())
    plan_path = STAGE / 'production_integration_plan.json'
    plan = json.loads(plan_path.read_text())
    if report.get('passed') is not True or report.get('source_stable') is not True or report.get('production_applied') is not False:
        raise ValueError('Exact native integration objects must pass unchanged')
    if len(report['object_sha256']) != 13 or len(plan['production_sources']) != 14 or len(plan['source_owned_fragment_targets']) != 12:
        raise ValueError('Unexpected native source/owner inventory')
    for name, expected in report['source_sha256'].items():
        source = ROOT / name
        if not source.resolve().is_relative_to(ROOT) or digest(source.read_bytes()) != expected:
            raise ValueError('Typechecked input changed: ' + name)
    for name, expected in plan['production_sources'].items():
        if digest((ROOT / name).read_bytes()) != expected:
            raise ValueError('Actual native owner changed: ' + name)
    prepared = Path(plan['prepared_native_root']).resolve()
    if prepared != STAGE / 'proposed-native':
        raise ValueError('Prepared sources outside reviewed staging')
    names = sorted(set(plan['production_sources']) | {'engine/src/khz/' + value for value in plan['source_owned_fragment_targets'].values()})
    patch = []
    outputs = {}
    for name in names:
        destination = ROOT / name
        exists = name in plan['production_sources']
        if not exists and destination.exists():
            raise ValueError('Refuse existing new native fragment: ' + name)
        before = destination.read_text(encoding='utf8') if exists else ''
        after = (prepared / name).read_text(encoding='utf8')
        patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                     fromfile='a/' + name if exists else '/dev/null', tofile='b/' + name))
        raw = destination.read_bytes() if exists else b''
        newline = '\r\n' if b'\r\n' in raw else '\n'
        encoded = after.lstrip('\ufeff').replace('\n', newline).encode('utf8')
        if raw.startswith(b'\xef\xbb\xbf'):
            encoded = b'\xef\xbb\xbf' + encoded
        outputs[name] = encoded
    if ''.join(patch) != (STAGE / 'production_integration.patch').read_text(encoding='utf8'):
        raise ValueError('Prepared source differs from reviewed exact patch')
    backup = STAGE / 'before74'
    if backup.exists():
        raise ValueError('Original native bytes already preserved; refuse reapplication')
    backup.mkdir()
    for name in plan['production_sources']:
        target = backup / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / name).read_bytes())
    result = {'schema': 1, 'applied': True, 'game_built': False, 'recorded_utc': datetime.now(timezone.utc).isoformat(),
              'typecheck': str(report_path.relative_to(ROOT)), 'typecheck_sha256': digest(report_path.read_bytes()),
              'plan_sha256': digest(plan_path.read_bytes()), 'patch_sha256': digest((STAGE / 'production_integration.patch').read_bytes()),
              'before_sha256': plan['production_sources'], 'after_sha256': {name: digest(raw) for name, raw in outputs.items()},
              'full_world_restore': False, 'authoritative_replay': False}
    for name, raw in outputs.items():
        (ROOT / name).write_bytes(raw)
    output = STAGE / 'production_application_build74.json'
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'applied': True, 'native_sources': len(outputs), 'result': str(output)}))

if __name__ == '__main__':
    main()
