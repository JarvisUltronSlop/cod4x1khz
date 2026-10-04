"""Review-only formatter candidates from actual Build73 identity interop calls.

Unlike the earlier two-startup fixture, this evidence has one fresh-default
startup and tests both real runtime modes. It never claims configured-zero
startup or persistence, publishes config, launches a game, or edits an archive.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

from plugin_identity_contract_native_probe import project_file, project_path

ROOT = Path(__file__).resolve().parents[1]


def main(archive_name, result_name):
    def local(name):
        return project_file(name).relative_to(ROOT).as_posix()

    def read(name):
        return json.loads(project_file(name).read_text(encoding='utf-8-sig'))

    def digest(name):
        return hashlib.sha256(project_file(name).read_bytes()).hexdigest()

    def reference(name):
        name = local(name)
        return {'path': name, 'sha256': digest(name)}

    def immutable(name, value):
        path = project_path(name)
        encoded = (json.dumps(value, indent=2) + '\n').encode('utf-8')
        if path.exists():
            if path.read_bytes() != encoded:
                raise RuntimeError('Changed immutable formatter candidate: ' + name)
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(encoded)

    archive_path, result_path = local(archive_name), local(result_name)
    archive, result = read(archive_path), read(result_path)
    build = archive.get('build')
    if (type(build) is not int or build <= 0 or archive.get('tests', {}).get('passed') is not True or
        result.get('build') != build or result.get('passed') is not True or
        not result.get('normal_cleanup') or not result.get('owned_processes_exited') or
        result.get('archive_mismatches') or local(result.get('build_archive', '')) != archive_path or
        result.get('fixture_registration', {}).get('observed_count') != 20 or
        not result.get('home', {}).get('absent_before_launch') or
        result.get('home', {}).get('format_dvar_override') is not None):
        raise ValueError('Fresh actual formatter calls must bind passing archive and normal cleanup')
    for path, expected in result['sha256'].items():
        if digest(path) != expected:
            raise ValueError('Actual native input or artifact changed: ' + path)
    server, dll = 'runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll'
    for path in (server, dll):
        if digest(path) != archive['binary_sha256'][path]:
            raise ValueError('Current archived formatter binary differs: ' + path)
    sources = ('engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_admin_server.cpp',
        'engine/src/khz/khz_controls.cpp', 'engine/src/khz/khz_controls.h',
        'native/steam_identity_format.hpp', 'tests/steam_identity_format_tests.cpp',
        'sdk/tests/native_identity_format_probe.inc', 'sdk/tests/regression_plugin.c')
    for path in sources:
        if digest(path) != archive['source_sha256'][path]:
            raise ValueError('Compiled formatter implementation/fixture differs: ' + path)
    markers = result.get('default_markers', {})
    expected = {
        'selected': {'cases': 99, 'failures': 0, 'initial': 1, 'restored': 1, 'provider_authentication': 0},
        'explicit64': {'cases': 96, 'failures': 0, 'provider_authentication': 0},
        'steam3': {'cases': 128, 'failures': 0, 'provider_authentication': 0}}
    if any(markers.get(name, {}).get('fields') != fields for name, fields in expected.items()):
        raise ValueError('Fresh loaded C-DLL formatter markers differ')
    log = local(archive['tests']['log'])
    if not re.search(r'Test\s+#\d+:\s+khz_steam_identity_format\s+[^\r\n]*Passed',
                     project_file(log).read_text(encoding='utf-8', errors='replace')):
        raise ValueError('Archived native formatter helper suite did not pass')
    config = read('config/public-capability-evidence.json')
    census = {row['id']: row for row in read('docs/public-capabilities.json')['capabilities']}
    output = f'docs/evidence/build{build}-sdk-identity-format'
    if project_path(output).exists():
        raise FileExistsError('Immutable formatter candidate already exists')
    writer = local(__file__)
    fixture = 'sdk/tests/native_identity_format_probe.inc'
    fixture_text = project_file(fixture).read_text(encoding='utf-8')
    updates, entries = {}, []

    def implementation(path, symbol):
        lines = project_file(path).read_text(encoding='utf-8').splitlines()
        matches = [number for number, line in enumerate(lines, 1)
                   if re.search(r'\b' + re.escape(symbol) + r'\b', line)]
        if not matches:
            raise ValueError('Actual implementation symbol absent: ' + symbol)
        return {'path': path, 'line': matches[0], 'symbol': symbol,
                'sha256': archive['source_sha256'][path]}

    def case(name, assertions, marker, label=None):
        if label and '"' + label + '"' not in fixture_text:
            raise ValueError('Compiled formatter assertion group absent: ' + label)
        return {'name': name, 'passed': True, 'assertions': assertions,
                'compiled_fixture': reference(fixture), 'fixture_assertion_group': label,
                'evidence': reference(result_path), 'observed': markers[marker]}

    def record(name, kind, contract, implementation_sources, cases, pending=()):
        capability = 'server:' + kind + ':' + name
        if capability not in census:
            raise ValueError('Exact formatter capability absent: ' + capability)
        path = output + '/' + name + '.json'
        proof = {'kind': 'semantic_regression', 'schema': 1, 'build': build,
            'protocol': 10002, 'sdk_version': 2, 'capability_id': capability,
            'passed': True, 'coverage': 'partial' if pending else 'reviewed_capability',
            'publication_state': 'candidate_requires_root_review',
            'semantic_contract': contract, 'cases': cases,
            'source_sha256': {row['path']: row['sha256'] for row in implementation_sources},
            'binary_sha256': {'server': archive['binary_sha256'][server]},
            'regression_dll_sha256': archive['binary_sha256'][dll],
            'build_archive': reference(archive_path), 'native_result': reference(result_path),
            'native_logs': [reference(Path(result_path).parent / filename)
                            for filename in ('server-stdio.log', 'native-server.log')],
            'proof_writer': reference(writer),
            'postarchive_tooling': 'Independently hashed review-only writer; coherent archive unchanged.',
            'startup_scope': 'One fresh default1 home only; both live runtime modes exercised by the C DLL.',
            'startup_configured_zero_proven': False, 'saved_config_persistence_proven': False,
            'pending_semantics': list(pending), 'official_authentication_proven': False,
            'full_public_feature_parity': False,
            'safety_adaptations': ['Refuse decimal twenty-digit overflow at capacity20 instead of pinned unchecked sprintf overrun.',
                                  'Null outputs are safe instead of dereferenced.']}
        immutable(path, proof)
        claim = {'semantic_contract': contract, 'implementation': implementation_sources,
                 'partial_regression' if pending else 'regression': [reference(path)]}
        if pending:
            claim['pending_semantics'] = list(pending)
        updates[capability] = claim
        entries.append({'capability_id': capability, 'coverage': proof['coverage'],
                        'artifact': reference(path), 'pending_semantics': list(pending)})

    host, admin, helper = 'engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_admin_server.cpp', 'native/steam_identity_format.hpp'
    selected = [case('Apply and read both real synchronized runtime dvar modes', 2, 'selected', 'configured_mode_applied'),
        case('Six independent literal IDs by eight capacities under each runtime mode; bounded buffers and terminators',
             96, 'selected', 'selected_dvar_format_bounds'),
        case('Restore original fresh-default setting', 1, 'selected', 'original_dvar_restored'),
        case('Twelve allowed universes by ten type spellings with maximum account', 120, 'steam3', 'all_allowed_universes_and_account_types'),
        case('Five invalid-universe boundaries return invalid Steam3 spelling', 5, 'steam3', 'invalid_universe_is_invalid_steam3'),
        case('Three invalid-type boundaries return invalid Steam3 spelling', 3, 'steam3', 'invalid_account_type_is_invalid_steam3'),
        case('Both modes complete after null output requests at positive, zero, negative capacities', 6, 'selected')]
    explicit = [case('Six literal uint64 IDs by eight capacities under both modes; explicit decimal ignores dvar and preserves guards',
                     96, 'explicit64', 'explicit64_ignores_mode_minimum_and_overflow_bounds'),
        case('Both modes complete after null explicit output requests at positive, zero, negative capacities', 6, 'explicit64')]
    supplement = {'name': 'Archived native helper suite independently passes golden/type/universe/null/minimum/overflow/sentinel checks',
        'passed': True, 'assertions': 1, 'evidence': reference(log),
        'fixture_source': reference('tests/steam_identity_format_tests.cpp'), 'test': 'khz_steam_identity_format'}
    record('Plugin_SteamIDToString', 'sdk_export',
        'Read the synchronized current sv_usesteam64id value: true emits whole uint64 decimal, false emits pinned Steam3 spelling with type/universe validation. '
        'Null/nonpositive outputs are safe. Decimal capacity below20 or insufficient capacity clears only byte0; twenty-digit values require21. '
        'Steam3 follows bounded truncation with a terminator. This is an export runtime contract; startup-config preservation and disk persistence are not proven here.',
        [implementation(host, 'Plugin_SteamIDToString'), implementation(helper, 'Selected'),
         implementation(helper, 'Steam3'), implementation(helper, 'Steam64'),
         implementation('engine/src/khz/khz_controls.h', 'KHZ_DvarScope'),
         implementation('engine/src/khz/khz_dvar_lock.cpp', 'KHZ_DvarEnter')], selected + [supplement])
    record('Plugin_SteamIDTo64String', 'sdk_export',
        'Always emit complete uint64 decimal regardless of current sv_usesteam64id. Null/nonpositive outputs write nothing. '
        'Positive capacity below20 clears only byte0; twenty-digit values require capacity21, so capacity20 refuses without touching the remaining bytes. '
        'Success terminates inside capacity and preserves surrounding caller storage.',
        [implementation(host, 'Plugin_SteamIDTo64String'), implementation(helper, 'Steam64')], explicit + [supplement])
    record('sv_usesteam64id', 'dvar',
        'Register true by default, expose synchronized live mode selection, preserve archive policy, and eventually preserve explicitly configured startup/saved values. '
        'Only fresh default1 and live0/1 selection/restoration are actually proven by this Build73 run.',
        [implementation(admin, 'KHZ_AdminInit')],
        [case('Previously absent project home starts with true and survives live selection/restoration', 3, 'selected')],
        pending=['Separate startup explicitly configured0 was not rerun on this binary.',
                 'Actual saved archived configuration and independent restart/reload persistence remain untested.'])
    immutable(output + '/capability-config-candidate.json', {'schema': 1, 'build': build,
        'target': 'config/public-capability-evidence.json', 'published': False, 'capabilities': updates})
    spec = importlib.util.spec_from_file_location('formatter_candidate_verifier', ROOT / 'scripts/capabilities.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    checked = {}
    for capability, claim in updates.items():
        status, reasons = verifier.verify_evidence(ROOT, census[capability], claim, config['binaries'])
        expected_status = 'pending' if capability.startswith('server:dvar:') else 'proven'
        if status != expected_status:
            raise ValueError((capability, status, reasons))
        checked[capability] = {'candidate_verifier_status': status, 'reasons': reasons, 'published_status_changed': False}
    immutable(output + '/validation.json', {'build': build, 'entries': checked,
        'verifier': reference('scripts/capabilities.py'), 'review_only': True})
    immutable(output + '/index.json', {'schema': 1, 'build': build,
        'build_archive': reference(archive_path), 'native_result': reference(result_path),
        'proof_writer': reference(writer), 'entries': entries,
        'complete_contract_candidates': 2, 'partial_pending_candidates': 1,
        'published': False, 'full_public_feature_parity': False})
    print(json.dumps({'published': False, 'candidate': output + '/capability-config-candidate.json',
        'complete_contract_candidates': 2, 'partial_pending_candidates': 1,
        'full_public_feature_parity': False}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True)
    parser.add_argument('--result', required=True)
    options = parser.parse_args()
    main(options.archive, options.result)
