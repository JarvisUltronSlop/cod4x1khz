"""Read-only exact pinned-source/current-ledger integration check.

Run only when the build owner authorizes offline validation. This never imports
or mutates the production scanner, configuration, census, native or binaries.
"""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
from finite_registration_enrollment import (CLIENT, ENTITY, FILTERS, HUD, REVIEWS,
                                           enroll_finite_registrations, _validate_source)

ROOT = Path(__file__).resolve().parents[3]


def main():
    report = {'scope': 'Staged finite source-identity enrollment only; no semantic promotion',
              'source_checks': [], 'errors': [], 'production_changed': False}
    manifest_path = ROOT / 'docs/public-capabilities.json'
    config_path = ROOT / 'config/public-capability-evidence.json'
    manifest_bytes = manifest_path.read_bytes()
    config_bytes = config_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    config = json.loads(config_bytes)
    original = {entry['id']: entry for entry in manifest['capabilities']}
    if len(original) != len(manifest['capabilities']):
        raise ValueError('Current ledger has duplicate capability IDs')
    report['inputs'] = {'manifest': {'path': 'docs/public-capabilities.json',
                                    'sha256': hashlib.sha256(manifest_bytes).hexdigest()},
                        'configuration': {'path': 'config/public-capability-evidence.json',
                                          'sha256': hashlib.sha256(config_bytes).hexdigest()}}
    for review in REVIEWS:
        raw = (ROOT / review.path).read_bytes()
        checked = {'family': review.family, 'path': review.path,
                   'sha256': hashlib.sha256(raw).hexdigest(),
                   'reviewed_sha256': review.sha256, 'table_line': review.table_line,
                   'call_lines': list(review.call_lines)}
        try:
            rows = _validate_source(review, raw)
            checked.update(passed=True, names=[row[0] for row in rows], row_count=len(rows))
        except (ValueError, UnicodeError) as error:
            checked.update(passed=False, error=str(error))
        report['source_checks'].append(checked)
    unchanged = deepcopy(original)
    target, errors = enroll_finite_registrations(original, ROOT, config['upstreams']['server'])
    report['errors'].extend(errors)
    if original != unchanged:
        report['errors'].append('Staged module mutated the caller input')
    expected_removed = {identifier for review in REVIEWS for identifier in review.consumed()}
    expected_added = {'server:dvar:' + name for name in FILTERS}
    expected_changed = {review.id_prefix + name for review in REVIEWS
                        if review.family != 'filters' for name in review.names}
    removed, added = set(original) - set(target), set(target) - set(original)
    changed = {identifier for identifier in set(original) & set(target)
               if original[identifier] != target[identifier]}
    field_metadata_unchanged = all(
        {key: value for key, value in original[identifier].items() if key != 'provenance'} ==
        {key: value for key, value in target[identifier].items()
         if key not in ('provenance', 'inventory_resolution')}
        for identifier in expected_changed if identifier in target)
    report['diff'] = {'removed': sorted(removed), 'added': sorted(added),
                      'changed_existing': sorted(changed),
                      'unrelated_changed': sorted(changed - expected_changed)}
    report['before'] = {'total': len(original),
                        'unresolved': sum(not value['resolved'] for value in original.values()),
                        'proven': sum(value['status'] == 'proven' for value in original.values())}
    report['after'] = {'total': len(target),
                       'unresolved': sum(not value['resolved'] for value in target.values()),
                       'proven': sum(value['status'] == 'proven' for value in target.values())}
    for passed, error in (
        (all(value['passed'] for value in report['source_checks']), 'Pinned native source validation failed'),
        (removed == expected_removed and len(removed) == 7, 'Removal set differs from reviewed seven opaque IDs'),
        (added == expected_added and len(added) == 4, 'Addition set differs from four pending filter IDs'),
        (changed == expected_changed and len(changed) == 49, 'Existing change set differs from 49 finite field IDs'),
        (field_metadata_unchanged, 'Existing finite field metadata changed beyond provenance/resolution'),
        (report['before'] == {'total': 2120, 'unresolved': 27, 'proven': 4}, 'Current corrected ledger differs from reviewed baseline'),
        (report['after'] == {'total': 2117, 'unresolved': 20, 'proven': 4}, 'Target finite counts differ'),
        (all(target[name]['status'] == original[name]['status'] and
             target[name]['applicable'] == original[name]['applicable'] for name in set(original) & set(target)),
         'Existing semantic status/applicability changed'),
        (all(target[name]['status'] == 'pending' and target[name]['applicable'] is True for name in added),
         'Added filter gained a semantic status/exclusion'),
        (manifest_path.read_bytes() == manifest_bytes and config_path.read_bytes() == config_bytes,
         'Production manifest/configuration changed during read-only validation')):
        if not passed:
            report['errors'].append(error)
    report['passed'] = not report['errors']
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
