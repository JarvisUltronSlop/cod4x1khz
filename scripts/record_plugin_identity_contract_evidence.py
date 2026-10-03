"""Write immutable, review-only classifier/GUID candidates from native results.

No game/test/build launch, archive rewrite, public-config merge, or promotion.
Two exact numeric classifier contracts are complete candidates. GUID conversion
remains partial because local crypto-provider failure was not injected.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

from plugin_identity_contract_native_probe import project_file, project_path

ROOT = Path(__file__).resolve().parents[1]


def main(archive_name, native_name):
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
        content = (json.dumps(value, indent=2) + '\n').encode('utf-8')
        path = project_path(name)
        if path.exists():
            if path.read_bytes() != content:
                raise RuntimeError('Refusing changed immutable candidate: ' + name)
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    archive_path, result_path = local(archive_name), local(native_name)
    archive, result = read(archive_path), read(result_path)
    build = archive.get('build')
    if type(build) is not int or build <= 0 or archive.get('tests', {}).get('passed') is not True:
        raise ValueError('Candidate requires the passing numbered native archive')
    if (result.get('passed') is not True or result.get('build') != build or
        not result.get('owned_processes_exited') or not result.get('normal_cleanup') or
        result.get('cleanup', {}).get('forced') is not False or result.get('archive_mismatches') or
        local(result.get('build_archive', '')) != archive_path or not result.get('cases') or
        any(case.get('passed') is not True for case in result['cases'])):
        raise ValueError('Candidate requires exact actual native behavior and normal owned cleanup')
    if result.get('fixture_registration', {}).get('observed_count') != 20:
        raise ValueError('Actual native fixture did not bind the twenty-command registry')
    for name, expected in result.get('sha256', {}).items():
        if digest(name) != expected:
            raise ValueError('Bound native input/artifact changed: ' + name)
    server, dll = 'runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll'
    for name in (server, dll):
        if digest(name) != archive.get('binary_sha256', {}).get(name):
            raise ValueError('Current binary does not match immutable native archive: ' + name)
    fixture = 'sdk/tests/native_identity_contract_probe.inc'
    goldens = 'sdk/tests/identity_guid_goldens.inc'
    host, identity = 'engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_identity.cpp'
    for name in (fixture, goldens, host, identity, 'sdk/tests/regression_plugin.c', 'sdk/include/pinc.h'):
        if digest(name) != archive.get('source_sha256', {}).get(name):
            raise ValueError('Actual compiled fixture/implementation differs from archive: ' + name)
    fixture_text = project_file(fixture).read_text(encoding='utf-8')
    observations = result.get('capability_observations', {})
    classifiers = ('Plugin_SteamIDIsIndividual', 'Plugin_SteamIDIsIndividualAndSteamAccount')
    classifier_fields = {'cases': 32838, 'failures': 0, 'matrix': 32768,
                         'bit_flips': 64, 'literals': 6, 'provider_authentication': 0}
    for name in classifiers:
        observed = observations.get(name, {})
        if (observed.get('actual_cases') != 32838 or observed.get('actual_failures') != 0 or
            observed.get('complete_contract_candidate') is not True or
            observed.get('reviewed_or_published_proven') is not False or
            observed.get('marker', {}).get('fields') != classifier_fields):
            raise ValueError('Exact live classifier matrix did not pass: ' + name)
    guid = observations.get('Plugin_GUID2PlayerID', {})
    guid_fields = {'cases': 247, 'failures': 0, 'goldens': 7, 'case_variants': 3,
                   'invalid_hex': 160, 'wrong_lengths': 64, 'null': 1,
                   'terminated_prefix': 1, 'provider_authentication': 0,
                   'provider_failure_injected': 0}
    if (guid.get('actual_cases') != 247 or guid.get('actual_failures') != 0 or
        guid.get('complete_contract_candidate') is not False or
        guid.get('marker', {}).get('fields') != guid_fields or
        result.get('oracle_review', {}).get('vectors') != 7 or
        result.get('oracle_review', {}).get('passed') is not True):
        raise ValueError('GUID native seven-golden/boundary partial scope did not pass')
    output = f'docs/evidence/build{build}-sdk-identity-contract'
    if project_path(output).exists():
        raise FileExistsError('Immutable identity candidate already exists')
    writer = local(__file__)
    census = {entry['id']: entry for entry in read('docs/public-capabilities.json')['capabilities']}
    config = read('config/public-capability-evidence.json')
    updates, entries = {}, []

    def implementation(path, symbol):
        lines = project_file(path).read_text(encoding='utf-8').splitlines()
        matches = [number for number, line in enumerate(lines, 1)
                   if re.search(r'\b' + re.escape(symbol) + r'\b', line)]
        if not matches:
            raise ValueError('Missing implementation symbol: ' + symbol)
        return {'path': path, 'line': matches[0], 'symbol': symbol,
                'sha256': archive['source_sha256'][path]}

    def case(name, assertions, label, observed):
        if '"' + label + '"' not in fixture_text:
            raise ValueError('Missing assertion group in compiled fixture: ' + label)
        return {'name': name, 'passed': True, 'assertions': assertions,
                'fixture_assertion_group': label, 'compiled_fixture': reference(fixture),
                'observed': observed, 'evidence': reference(result_path)}

    def record(name, contract, sources, cases, upstream_symbol, upstream_line, pending=(), adaptations=()):
        capability = 'server:sdk_export:' + name
        if capability not in census:
            raise ValueError('Exact declared capability missing from inventory: ' + capability)
        path = output + '/' + name + '.json'
        proof = {'kind': 'semantic_regression', 'schema': 1, 'build': build,
            'protocol': 10002, 'sdk_version': 2, 'capability_id': capability,
            'passed': True, 'coverage': 'partial' if pending else 'reviewed_capability',
            'publication_state': 'candidate_requires_root_review',
            'semantic_contract': contract, 'cases': cases,
            'source_sha256': {item['path']: item['sha256'] for item in sources},
            'binary_sha256': {'server': archive['binary_sha256'][server]},
            'regression_dll_sha256': archive['binary_sha256'][dll],
            'build_archive': reference(archive_path), 'native_result': reference(result_path),
            'native_logs': [reference(Path(result_path).parent / name)
                            for name in ('server-stdio.log', 'native-server.log')],
            'independent_oracle_review': reference(result['oracle_review']['path']),
            'compiled_fixture': reference(fixture), 'compiled_guid_goldens': reference(goldens),
            'harness': reference('scripts/plugin_identity_contract_native_probe.py'),
            'proof_writer': reference(writer),
            'postarchive_tooling': 'Harness/proof tooling is independently hashed; immutable build archive remains unchanged.',
            'upstream_contract': {'revision': 'ddd65fc', 'path': 'upstream/cod4x_server/src/sapi.c',
                'symbol': upstream_symbol, 'line': upstream_line,
                'source': reference('upstream/cod4x_server/src/sapi.c'),
                'export_alias': reference('upstream/cod4x_server/src/asmsource/pluginexports.asm')},
            'safety_adaptations': list(adaptations), 'pending_semantics': list(pending),
            'official_authentication_proven': False, 'full_public_feature_parity': False,
            'numeric_classification_only': not pending,
            'review': 'Actual rebuilt C DLL calls with independently assembled cases; no registry/name-only inference.'}
        immutable(path, proof)
        claim = {'semantic_contract': contract, 'implementation': sources,
            'partial_regression' if pending else 'regression': [reference(path)]}
        if pending:
            claim['pending_semantics'] = list(pending)
        updates[capability] = claim
        entries.append({'capability_id': capability, 'coverage': proof['coverage'],
            'artifact': reference(path), 'pending_semantics': list(pending)})

    for name, steam_only, upstream, line in (
        (classifiers[0], False, 'SV_SApiSteamIDIndividual', 245),
        (classifiers[1], True, 'SV_SApiSteamIDIndividualSteamOnly', 261)):
        contract = ('Classify the uint64 numeric bit fields exactly: type must equal 1 and the 20-bit instance must equal 1; '
            + ('universe must equal 1. ' if steam_only else 'universe must equal 1 or be 32 through 39 inclusive. ')
            + 'The low 32 account bits do not affect the result. Return the corresponding boolean; this does not authenticate an account or contact a service.')
        observed = observations[name]['marker']
        record(name, contract,
            [implementation(host, name), implementation(identity, 'SteamIdIndividual')],
            [case('Independently assemble all 256 universes, all 16 types, four instance boundaries, both account extremes',
                  32768, 'assembled_field_matrix', observed),
             case('Flip each of 64 bits of independent public-account literal and check account/private-universe effects',
                  64, 'independent_single_bit_oracle', observed),
             case('Independent literal zero/max/public/private/invalid-instance/invalid-type goldens',
                  6, 'literal_goldens', observed)], upstream, line)
    observed = guid['marker']
    record('Plugin_GUID2PlayerID',
        'Parse a NUL-terminated exact 32-character case-insensitive hexadecimal GUID without modifying caller storage. '
        'For valid input, derive four little-endian bytes with pinned 256-byte salt and PBKDF2-HMAC-SHA256/100, '
        'and assemble private universe32/type1/instance1 identifier. Malformed length/hex or null returns zero. '
        'Local cryptographic failure returns zero by implementation; its native failure branch is not proven here.',
        [implementation(host, 'Plugin_GUID2PlayerID'), implementation(identity, 'GuidToPlayerId'),
         implementation(identity, 'DecodeHex'), implementation(identity, 'hexValue')],
        [case('Seven literal independent Python/.NET PBKDF2 goldens in lowercase, uppercase, mixed case; entire input guard buffer unchanged',
              21, 'independent_pbkdf2_golden_case', observed),
         case('Five invalid hex classes at every one of 32 positions; zero return and entire input guard buffer unchanged',
              160, 'reject_nonhex_at_every_byte_without_mutating_input', observed),
         case('Every length 0 through64 except32; zero return and entire input guard buffer unchanged',
              64, 'reject_wrong_length_without_mutating_input', observed),
         case('Null pointer returns zero', 1, 'null_input_returns_zero', observed),
         case('Valid terminated prefix ignores and preserves following caller byte',
              1, 'valid_terminated_prefix_ignores_following_bytes', observed)],
        'SV_SApiGUID2PlayerID', 457,
        pending=['Actual local crypto-provider failure injection/return path remains untested.'],
        adaptations=['Malformed hexadecimal rejects instead of pinned partial strtol parsing.',
                     'Null input returns zero instead of pinned strlen dereference.',
                     'Local crypto failure returns zero instead of pinned private ID with account zero; this branch remains pending actual injection.'])
    immutable(output + '/capability-config-candidate.json', {'schema': 1, 'build': build,
        'target': 'config/public-capability-evidence.json', 'published': False, 'capabilities': updates})
    spec = importlib.util.spec_from_file_location('identity_candidate_verifier', ROOT / 'scripts/capabilities.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    checked = {}
    for capability, claim in updates.items():
        status, reasons = verifier.verify_evidence(ROOT, census[capability], claim, config['binaries'])
        expected = 'pending' if capability.endswith(':Plugin_GUID2PlayerID') else 'proven'
        if status != expected:
            raise ValueError((capability, status, reasons))
        checked[capability] = {'candidate_verifier_status': status, 'reasons': reasons,
                               'published_status_changed': False}
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
