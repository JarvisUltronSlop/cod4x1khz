"""Apply only the three reviewed native timestep changes after fixture freeze.

Retains exact Build73 source bytes for extraction against the real production
functions. Requires a passing, current source-bound native contract result.
"""
from pathlib import Path
import argparse
from datetime import datetime, timezone
import hashlib
import json
import proposals

ROOT = Path(__file__).resolve().parents[2]

def digest(data):
    return hashlib.sha256(data).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    contract = args.contract.resolve()
    if not contract.is_relative_to(ROOT / 'staged/timestep/results'):
        raise ValueError('Contract must be an owned staged timestep result')
    report = json.loads(contract.read_text())
    if report.get('passed') is not True or report.get('assertions', {}).get('assertions', 0) < 319:
        raise ValueError('Expanded current native contracts must pass before application')
    inputs = {name: (ROOT / name).read_bytes() for name in report['native_sources']}
    for name, raw in inputs.items():
        if digest(raw) != report['native_sources'][name]:
            raise ValueError('Native input changed after passing contract: ' + name)
    if digest((ROOT / 'staged/timestep/proposals.py').read_bytes()) != report['proposal_sha256']:
        raise ValueError('Reviewed proposal changed after contract')
    changes = proposals.proposed_sources()
    result = {'schema': 1, 'applied': args.apply, 'contract': str(contract.relative_to(ROOT)).replace('\\', '/'),
              'contract_sha256': digest(contract.read_bytes()), 'changes': {}}
    for relative, (before, after) in changes.items():
        raw = inputs[relative.as_posix()]
        newline = '\r\n' if b'\r\n' in raw else '\n'
        encoded = after.replace('\n', newline).encode('utf-8')
        if raw.startswith(b'\xef\xbb\xbf'):
            encoded = b'\xef\xbb\xbf' + encoded
        result['changes'][relative.as_posix()] = {'before': digest(raw), 'after': digest(encoded),
                                                  'production_output': encoded}
    if args.apply:
        baseline = ROOT / 'staged/timestep/before73'
        if baseline.exists():
            raise ValueError('Baseline already exists; do not reapply or overwrite its original bytes')
        baseline.mkdir()
        for name, raw in inputs.items():
            target = baseline / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
        (baseline / 'manifest.json').write_text(json.dumps({'schema': 1, 'build': 73, 'native_sources': report['native_sources']}, indent=2) + '\n')
        for name, value in result['changes'].items():
            (ROOT / name).write_bytes(value['production_output'])
    for value in result['changes'].values():
        del value['production_output']
    output = ROOT / 'staged/timestep/results' / ('integration-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f') + '.json')
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'applied': args.apply, 'result': str(output)}))

if __name__ == '__main__':
    main()
