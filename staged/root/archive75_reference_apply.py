"""Apply the two reviewed archive-oracle reference resets after native tests."""
from pathlib import Path
import difflib
import hashlib
import importlib.util
import json

ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / 'staged/archive'
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    evidence = STAGE / 'results/20261003-132024-942012/result.json'
    if digest(evidence) != '267ef11e8e5a4b8fa128389078835279b51de0c134c760c84d527e9f128f099b':
        raise ValueError('Reviewed native reference result changed')
    report = json.loads(evidence.read_text())
    if report.get('passed') is not True or report.get('source_stable') is not True or report.get('contracts_executed') is not True:
        raise ValueError('Actual legacy and packed native reference contracts must pass')
    for name, expected in report['source_sha256'].items():
        if digest(ROOT / name) != expected:
            raise ValueError('Source-bound native reference input changed: ' + name)
    patch = STAGE / 'reference_reset.patch'
    if digest(patch) != 'e161d31209ab1f44e478f8485e41ebfd2c0d2482e09abdb230d292f2bb697bb8':
        raise ValueError('Root-reviewed two-reset patch changed')
    spec = importlib.util.spec_from_file_location('reference_proposal', STAGE / 'prepare_reference_fix.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = ROOT / module.RELATIVE
    before = source.read_text(encoding='utf8')
    after = module.proposed(before)
    expected_patch = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
        fromfile='a/' + module.RELATIVE, tofile='b/' + module.RELATIVE))
    if expected_patch != patch.read_text():
        raise ValueError('Exact transformed source differs from reviewed patch')
    original = source.read_bytes()
    backup = STAGE / 'reference-reset-before75.inl'
    if backup.exists():
        raise ValueError('Original reference bytes already retained; refuse reapply')
    backup.write_bytes(original)
    newline = '\r\n' if b'\r\n' in original else '\n'
    result = after.replace('\n', newline).encode('utf8')
    source.write_bytes(result)
    output = STAGE / 'reference_reset_application75.json'
    output.write_text(json.dumps({'applied': True, 'native_codec_changed': False,
        'comparisons_weakened': False, 'game_built': False, 'evidence_sha256': digest(evidence),
        'patch_sha256': digest(patch), 'before_sha256': hashlib.sha256(original).hexdigest(),
        'after_sha256': digest(source)}, indent=2) + '\n')
    print(json.dumps({'applied': True, 'result': str(output)}))

if __name__ == '__main__':
    main()
