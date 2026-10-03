"""Prepare reviewed two-export formatter evidence from actual native results.

This post-archive proof tool never launches tests/games or modifies an archive.
Default mode writes immutable scoped candidates; --publish merges only these
three reviewed entries after root approves them. Run only outside timing.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main(archive_name, result_name, publish=False):
    def local(name):
        path = Path(name)
        if not path.is_absolute():
            path = ROOT / path
        path = path.resolve()
        if not path.is_relative_to(ROOT):
            raise ValueError('Evidence must remain beneath project root')
        return path.relative_to(ROOT).as_posix()

    def read(name):
        return json.loads((ROOT / name).read_text(encoding='utf-8-sig'))

    def digest(name):
        return hashlib.sha256((ROOT / name).read_bytes()).hexdigest()

    def reference(name):
        return {'path': name, 'sha256': digest(name)}

    def immutable(name, value):
        content = (json.dumps(value, indent=2) + '\n').encode('utf-8')
        path = ROOT / name
        if path.exists():
            if path.read_bytes() != content:
                raise RuntimeError('Refusing changed immutable proof artifact: ' + name)
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    archive_path, result_path = local(archive_name), local(result_name)
    archive, result = read(archive_path), read(result_path)
    build = archive.get('build')
    if type(build) is not int or build <= 0 or archive.get('tests', {}).get('passed') is not True:
        raise ValueError('Formatter proof requires an actual numbered passing archive')
    if not result.get('passed') or not result.get('owned_processes_exited') or result.get('build') != build or result.get('archive_mismatches'):
        raise ValueError('Formatter proof requires actual passing bound native behavior and exact cleanup')
    if local(result.get('build_archive', '')) != archive_path:
        raise ValueError('Native formatter result belongs to another archive')
    for path, expected in result['sha256'].items():
        if digest(local(path)) != expected:
            raise ValueError('Native formatter input/hash changed: ' + path)
    server, dll = 'runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll'
    for path in (server, dll):
        if digest(path) != archive['binary_sha256'][path]:
            raise ValueError('Formatter proof requires the current archived binary: ' + path)
    if result.get('fixture_registration', {}).get('observed_count') != 20:
        raise ValueError('Native fixture did not enforce the actual twenty-command inventory')
    if len(result.get('phases', [])) != 2 or not result.get('cases') or not all(case.get('passed') is True for case in result['cases']):
        raise ValueError('Both actual formatter phases and all assertions must pass')
    phases = {phase['name']: phase for phase in result['phases']}
    for name, initial in (('fresh-default', 1), ('configured-zero', 0)):
        phase = phases.get(name, {})
        if not phase.get('home_absent_before_launch') or not phase.get('owned_process_exited') or phase.get('expected_initial') != initial:
            raise ValueError('Fresh native formatter phase/lifetime was not proven: ' + name)
        for marker, count in (('KHZ_IDENTITY_FORMAT_SELECTED', 99), ('KHZ_IDENTITY_FORMAT_EXPLICIT64', 96), ('KHZ_IDENTITY_FORMAT_STEAM3', 128)):
            fields = phase.get('markers', {}).get(marker, {}).get('fields', {})
            if fields.get('cases') != count or fields.get('failures') != 0 or fields.get('provider_authentication') != 0:
                raise ValueError('Native formatter marker did not pass its exact semantic scope: ' + marker)
        selected = phase['markers']['KHZ_IDENTITY_FORMAT_SELECTED']['fields']
        if selected.get('initial') != initial or selected.get('restored') != 1:
            raise ValueError('Native configured mode/restoration failed')
    source_names = ('engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_admin_server.cpp',
                    'engine/src/khz/khz_controls.cpp', 'engine/src/khz/khz_controls.h',
                    'native/steam_identity_format.hpp', 'tests/steam_identity_format_tests.cpp',
                    'sdk/tests/regression_plugin.c', 'sdk/tests/native_identity_format_probe.inc', 'sdk/tests/native_stats_probe.inc')
    for name in source_names:
        if digest(name) != archive['source_sha256'][name]:
            raise ValueError('Reviewed formatter source differs from archive: ' + name)
    log_path = local(archive['tests']['log'])
    if not re.search(r'Test\s+#\d+:\s+khz_steam_identity_format\s+[^\r\n]*Passed', (ROOT / log_path).read_text(encoding='utf-8', errors='replace')):
        raise ValueError('Actual archived native formatter suite did not pass')
    fixture_path = 'sdk/tests/native_identity_format_probe.inc'
    fixture_text = (ROOT / fixture_path).read_text(encoding='utf-8')
    output = f'docs/evidence/build{build}-sdk-identity-format'
    if (ROOT / output).exists() and not publish:
        raise FileExistsError('Immutable formatter candidate exists; --publish revalidates the unchanged candidate only')
    writer_path = local(__file__)
    config_path = 'config/public-capability-evidence.json'
    config = read(config_path)
    census = {entry['id']: entry for entry in read('docs/public-capabilities.json')['capabilities']}
    updates, entries = {}, []

    def implementation(name, symbol):
        lines = (ROOT / name).read_text(encoding='utf-8').splitlines()
        matches = [index for index, line in enumerate(lines, 1) if re.search(r'\b' + re.escape(symbol) + r'\b', line)]
        if not matches:
            raise ValueError('Missing reviewed formatter implementation symbol: ' + symbol)
        return {'path': name, 'sha256': archive['source_sha256'][name], 'line': matches[0], 'symbol': symbol}

    def case(name, assertions, phase=None, marker=None, fixture_label=None):
        if fixture_label and '"' + fixture_label + '"' not in fixture_text:
            raise ValueError('Assertion group absent from the actual compiled formatter fixture: ' + fixture_label)
        item = {'name': name, 'passed': True, 'assertions': assertions, 'evidence': reference(result_path),
                'compiled_fixture': reference(fixture_path)}
        if phase:
            item['phase'] = phase
        if marker:
            item['observed'] = phases[phase]['markers'][marker]
        if fixture_label:
            item['fixture_assertion_group'] = fixture_label
        return item

    def supplement():
        return {'name': 'Actual native formatter guard-byte/golden/null/type/universe suite', 'passed': True, 'assertions': 1,
                'evidence': reference(log_path), 'test': 'khz_steam_identity_format',
                'fixture_source': reference('tests/steam_identity_format_tests.cpp')}

    def record(name, kind, contract, sources, cases, pending=(), adaptations=()):
        identity = 'server:' + kind + ':' + name
        if identity not in census:
            raise ValueError('Uninventoried formatter capability: ' + identity)
        path = output + '/' + name + '.json'
        proof = {'kind': 'semantic_regression', 'schema': 1, 'build': build, 'protocol': 10002, 'sdk_version': 2,
                 'capability_id': identity, 'passed': True, 'coverage': 'partial' if pending else 'reviewed_capability',
                 'semantic_contract': contract, 'cases': cases,
                 'source_sha256': {source['path']: source['sha256'] for source in sources},
                 'binary_sha256': {'server': archive['binary_sha256'][server]},
                 'regression_dll_sha256': archive['binary_sha256'][dll], 'build_archive': reference(archive_path),
                 'compiled_fixture_source_archive': reference(archive_path), 'native_result': reference(result_path),
                 'native_logs': [reference(local((ROOT / result_path).parent / (phase + '-stdio.log'))) for phase in phases],
                 'proof_writer': reference(writer_path),
                 'proof_writer_provenance': 'Post-archive proof tooling, independently hashed; never overwrites or claims inclusion in the coherent compiled-source archive',
                 'safety_adaptations': list(adaptations), 'pending_semantics': list(pending),
                 'official_authentication_proven': False, 'full_public_feature_parity': False,
                 'review': 'Actual loaded-C-DLL boundary behavior plus independently native-tested helper; syntax/registration/names are not semantic proof'}
        immutable(path, proof)
        claim = {'semantic_contract': contract, 'implementation': sources,
                 'partial_regression' if pending else 'regression': [reference(path)]}
        if pending:
            claim['pending_semantics'] = list(pending)
        updates[identity] = claim
        entries.append({'capability_id': identity, 'coverage': proof['coverage'], 'artifact': reference(path), 'pending_semantics': list(pending)})

    selected_cases, explicit_cases, dvar_cases = [], [], []
    for phase in phases:
        selected_cases += [case(phase + ': switch both real dvar modes', 2, phase, 'KHZ_IDENTITY_FORMAT_SELECTED', 'configured_mode_applied'),
                           case(phase + ': six identifier goldens by eight capacities under both modes', 96, phase, 'KHZ_IDENTITY_FORMAT_SELECTED', 'selected_dvar_format_bounds'),
                           case(phase + ': preserve original configured mode', 1, phase, 'KHZ_IDENTITY_FORMAT_SELECTED', 'original_dvar_restored'),
                           case(phase + ': every allowed universe/type at account maximum and invalid classifications', 128, phase, 'KHZ_IDENTITY_FORMAT_STEAM3'),
                           case(phase + ': null outputs with positive/zero/negative lengths survive and reporting completes', 1, phase, 'KHZ_IDENTITY_FORMAT_SELECTED')]
        explicit_cases += [case(phase + ': explicit64 ignores mode and checks minimum/overflow/sentinels for all six goldens', 96, phase, 'KHZ_IDENTITY_FORMAT_EXPLICIT64', 'explicit64_ignores_mode_minimum_and_overflow_bounds'),
                           case(phase + ': null explicit64 outputs survive and reporting completes', 1, phase, 'KHZ_IDENTITY_FORMAT_EXPLICIT64')]
        dvar_cases += [case(phase + ': fresh default or startup-configured zero survives registration and restores', 1, phase, 'KHZ_IDENTITY_FORMAT_SELECTED')]
    host, admin, helper = 'engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_admin_server.cpp', 'native/steam_identity_format.hpp'
    selected_contract = 'Read synchronized sv_usesteam64id and select whole Steam64 decimal or pinned Steam3 spelling with allowed universe/type validation and bounded output. Preserve startup-configured values and original fixture setting; null/nonpositive outputs are safe. Decimal mode follows pinned capacity-below20 refusal and safely rejects twenty-digit values at capacity20.'
    explicit_contract = 'Always emit complete uint64 decimal independently of sv_usesteam64id. Positive capacity below20 clears only byte0; null/nonpositive requests write nothing. Values needing twenty digits require capacity21, with capacity20 refusal clearing only byte0; successful outputs include their terminator and never cross capacity.'
    adaptations = ['Pinned unchecked sprintf overflows a capacity20 buffer for twenty-digit uint64; adapted implementation refuses rather than overruns', 'Null output is safe rather than dereferenced']
    record('Plugin_SteamIDToString', 'sdk_export', selected_contract,
           [implementation(host, 'Plugin_SteamIDToString'), implementation(admin, 'KHZ_AdminInit'),
            implementation(helper, 'Selected'), implementation(helper, 'Steam3'), implementation(helper, 'Steam64'),
            implementation('engine/src/khz/khz_controls.h', 'KHZ_DvarScope'), implementation('engine/src/khz/khz_controls.cpp', 'KHZ_DvarEnter')],
           selected_cases + [supplement()], adaptations=adaptations)
    record('Plugin_SteamIDTo64String', 'sdk_export', explicit_contract,
           [implementation(host, 'Plugin_SteamIDTo64String'), implementation(helper, 'Steam64')],
           explicit_cases + [supplement()], adaptations=adaptations)
    record('sv_usesteam64id', 'dvar', 'Register true by default with archive semantics, preserve explicitly configured values, and select public identifier formatting. Actual saved-config persistence remains required.',
           [implementation(admin, 'KHZ_AdminInit')], dvar_cases,
           pending=['Actual saved archived configuration and a separate restart/reload preserving its value have not been exercised'])
    candidate = {'schema': 1, 'build': build, 'target': config_path, 'capabilities': updates}
    immutable(output + '/capability-config-candidate.json', candidate)
    spec = importlib.util.spec_from_file_location('capabilities', ROOT / 'scripts/capabilities.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    checked = {}
    for identity, claim in updates.items():
        status, reasons = verifier.verify_evidence(ROOT, census[identity], claim, config['binaries'])
        expected = 'pending' if identity.startswith('server:dvar:') else 'proven'
        if status != expected:
            raise ValueError((identity, status, reasons))
        checked[identity] = {'status': status, 'reasons': reasons}
    immutable(output + '/validation.json', {'build': build, 'entries': checked, 'verifier': reference('scripts/capabilities.py')})
    immutable(output + '/index.json', {'schema': 1, 'build': build, 'build_archive': reference(archive_path),
              'native_result': reference(result_path), 'proof_writer': reference(writer_path), 'entries': entries,
              'complete_evidence_entries': 2, 'partial_pending_entries': 1, 'full_public_feature_parity': False})
    if publish:
        # Scoped merge keeps newly reviewed network entries and all unrelated
        # capabilities intact even when another candidate was prepared first.
        latest = read(config_path)
        latest.setdefault('capabilities', {}).update(updates)
        (ROOT / config_path).write_text(json.dumps(latest, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'published': publish, 'candidate': output + '/capability-config-candidate.json',
                      'complete_evidence_entries': 2, 'partial_pending_entries': 1, 'full_public_feature_parity': False}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True)
    parser.add_argument('--result', required=True)
    parser.add_argument('--publish', action='store_true', help='Merge only reviewed entries after root approves the immutable candidate')
    args = parser.parse_args()
    main(args.archive, args.result, args.publish)
