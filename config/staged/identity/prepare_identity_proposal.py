"""Guarded proposal preparation only; never applies a production patch.

Run after the build owner authorizes offline source checks, not during native
fixtures/timing. Every source must match the fixed coherent Build74 snapshot.
The output is a reviewable unified patch plus new proposed fixture data; no
SDK/native/scripts/config-census input is modified.
"""
from collections import Counter
import difflib
import hashlib
import json
from pathlib import Path
import re
from parser_contract_cases import native_cases

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
BASE = {
    'engine/src/khz/khz_identity.cpp': 'bcf2d1e0b185f790008c67ec5638005b1aac1936c11eabbab156d2d58b0f0bc4',
    'engine/src/khz/khz_identity.h': '658dabe1719bec354e7a24ad2f59d6acbd7f332ecdda1d334b65bb6c8cd42463',
    'sdk/tests/regression_plugin.c': 'cf224e80e4b5c3405d1263810f856c70b1b91ef3a79bd9e7a229583f1ba996d0',
    'engine/src/khz/khz_plugins.cpp': '8f729acff8ecd817e39bddc51b96b3a717a295188bc675f48aaf8e8163ffdfec',
}


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Expected exactly one original anchor: ' + old[:80])
    return text.replace(old, new, 1)


def fragment(name):
    return (HERE / name).read_text(encoding='utf8')


def c_string(text):
    if text is None:
        return 'NULL'
    out = '"'
    for char in text:
        if char in ('"', '\\'):
            out += '\\' + char
        elif not 32 <= ord(char) <= 126:
            if ord(char) >= 128:
                raise ValueError('Native literal case is not reviewed ASCII')
            out += '\\' + format(ord(char), '03o')
        else:
            out += char
    return out + '"'


def command_names(text):
    names = re.findall(r'\bPlugin_AddCommand\s*\(\s*"([^"]+)"', text)
    if len(names) != 20 or len(set(names)) != 20:
        raise ValueError('Fixture must keep twenty distinct literal command registrations')
    return names


def native_fixture():
    categories = ('pinned-valid', 'bug-correction-four-fields', 'removed-extension')
    records = native_cases()
    labels = [case.label for case in records]
    if len(set(labels)) != len(labels):
        raise ValueError('Duplicate native parser label')
    rows = []
    counts = Counter()
    for case in records:
        kind = (categories.index(case.contract) if case.contract in categories else
                3 if case.contract.startswith('safety-') else 4)
        counts[kind] += 1
        rows.append('    {' + ', '.join((c_string(case.label), c_string(case.text),
                    '0x' + format(case.expected, '016x') + 'ULL', str(kind))) + '},')
    prelude = '''/* Generated independent literal expectations. Not semantic proof until
 * this rebuilt DLL actually executes on the archive-bound native server. */
struct identity_parser_literal {
    const char* label; const char* text; uint64_t expected; unsigned int kind;
};
static const struct identity_parser_literal identity_parser_literals[] = {
'''
    end = '''
};
static int identity_parse_readonly(const struct identity_parser_literal* literal) {
    unsigned char storage[160], original[160];
    char* input = (char*)storage + 8;
    size_t length;
    if (!literal->text) return Plugin_StringToSteamID(NULL) == literal->expected;
    length = strlen(literal->text);
    if (length + 9 > sizeof(storage)) return 0;
    memset(storage, 0xa5, sizeof(storage));
    memcpy(input, literal->text, length + 1);
    memcpy(original, storage, sizeof(storage));
    return Plugin_StringToSteamID(input) == literal->expected &&
        !memcmp(storage, original, sizeof(storage));
}
static void identity_parse_probe(void) {
    struct identity_contract_counts result = {0,0};
    unsigned int kinds[5] = {0,0,0,0,0};
    unsigned int i;
    char terminated[48], original[48];
    unsigned int guardFaults = 0;
    for (i = 0; i != sizeof(identity_parser_literals)/sizeof(identity_parser_literals[0]); ++i) {
        const struct identity_parser_literal* literal = &identity_parser_literals[i];
        ++kinds[literal->kind];
        identity_contract_check(&result, identity_parse_readonly(literal), literal->label);
    }
    memset(terminated, 'z', sizeof(terminated));
    memcpy(terminated, "76561197960265770", 18);
    memcpy(original, terminated, sizeof(original));
    identity_contract_check(&result, Plugin_StringToSteamID(terminated) == 0x011000010000002aULL &&
        !memcmp(terminated, original, sizeof(original)), "terminated-prefix");
    identity_parse_guard_pages(&result, &guardFaults);
    Plugin_Printf("KHZ_IDENTITY_PARSE cases=%u failures=%u pinned_valid=%u bug_correction=%u "
        "removed_extension=%u safety=%u malformed=%u terminated_prefix=1 "
        "guard_page_cases=5 guard_page_faults=%u guard_page_cleanup=1 provider_authentication=0\\n",
        result.cases, result.failures, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], guardFaults);
}
'''
    # Put helpers after the literal array closes and before the probe entry.
    guards = fragment('parser_guard_pages.proposed.inc')
    end = end.replace('static void identity_parse_probe(void)', guards + '\nstatic void identity_parse_probe(void)', 1)
    return prelude + '\n'.join(rows) + end, counts, len(records) + 7


def main():
    old, new = {}, {}
    for relative, expected in BASE.items():
        raw = (ROOT / relative).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError('Coherent original source changed: ' + relative)
        old[relative] = raw.decode('utf8').replace('\r\n', '\n')
        new[relative] = old[relative]
    cpp = new['engine/src/khz/khz_identity.cpp']
    gate = '#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)\n'
    cpp = once(cpp, '#include <wincrypt.h>\n', '#include <wincrypt.h>\n' + gate +
        '#include <qcommon/cmd.h>\n#include <qcommon/qcommon.h>\n#include <qcommon/threads.h>\n#include <server_mp/server_mp.h>\n#endif\n')
    cpp = once(cpp, 'unsigned accountType(char c) {', fragment('field_number.proposed.inc') + '\nunsigned accountType(char c) {')
    start, end = cpp.index('std::uint64_t StringToSteamId('), cpp.index('bool SteamIdToString(')
    cpp = cpp[:start] + fragment('string_to_steamid.proposed.inc') + '\n' + cpp[end:]
    cpp = once(cpp, '};\n}\nPrivateKey::~PrivateKey()', '};\n' + fragment('guid_operations.proposed.inc') + '\n}\nPrivateKey::~PrivateKey()')
    cpp = once(cpp, 'Algorithm algorithm; std::array<std::uint8_t, 4> derived{};\n'
        '    if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0 ||\n'
        '        BCryptDeriveKeyPBKDF2(algorithm.handle, decoded.data(), ULONG(decoded.size()), const_cast<PUCHAR>(guidSalt), sizeof(guidSalt), 100,\n'
        '            derived.data(), ULONG(derived.size()), 0) < 0) return 0;',
        'GuidAlgorithm algorithm; std::array<std::uint8_t, 4> derived{};\n'
        '    if (guidOpenAlgorithm(algorithm) < 0 || guidDeriveKey(algorithm, decoded, derived) < 0) return 0;')
    cpp += '\n' + fragment('guid_probe_command.proposed.inc')
    new['engine/src/khz/khz_identity.cpp'] = cpp
    new['engine/src/khz/khz_identity.h'] += '\n' + gate + '// Internal gated native diagnostic initialization; no public SDK export.\nbool KHZ_InitIdentityProbe();\n#endif\n'
    fixture = new['sdk/tests/regression_plugin.c']
    fixture = once(fixture, '#include "native_identity_contract_probe.inc"',
                   '#include "native_identity_contract_probe.inc"\n#include "native_identity_parser_probe.inc"')
    fixture = once(fixture, '    else Plugin_PrintError("KHZ_IDENTITY_DISPATCH',
        '    else if (Plugin_Cmd_Argc() == 2 && !strcmp(Plugin_Cmd_Argv(1), "parse")) identity_parse_probe();\n'
        '    else Plugin_PrintError("KHZ_IDENTITY_DISPATCH')
    fixture = once(fixture, 'expected_subcommand=format|classify|guid\\n',
                   'expected_subcommand=format|classify|guid|parse\\n')
    registered_names = command_names(fixture)
    new['sdk/tests/regression_plugin.c'] = fixture
    new['engine/src/khz/khz_plugins.cpp'] = once(new['engine/src/khz/khz_plugins.cpp'],
        'KHZ_API uint64_t __cdecl Plugin_StringToSteamID(const char* value) { return value ? khz::identity::StringToSteamId(value) : 0; }',
        'KHZ_API uint64_t __cdecl Plugin_StringToSteamID(const char* value) {\n'
        '    if (!value) return 0;\n'
        '    std::size_t length = 0;\n'
        '    while (length != 128 && value[length]) ++length;\n'
        '    if (length == 128) return 0;\n'
        '    return khz::identity::StringToSteamId(std::string_view(value, length));\n}')
    generated, kinds, total = native_fixture()
    new['sdk/tests/native_identity_parser_probe.inc'] = generated
    output = HERE / 'proposal'
    output.mkdir(exist_ok=True)
    patch = ''
    for relative, proposed in new.items():
        patch += ''.join(difflib.unified_diff(old.get(relative, '').splitlines(True),
            proposed.splitlines(True), fromfile='a/' + relative if relative in old else '/dev/null',
            tofile='b/' + relative))
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(proposed, encoding='utf8', newline='\n')
    (output / 'identity.patch').write_text(patch, encoding='utf8', newline='\n')
    contract = {'staged_only': True, 'production_changed': False, 'base_build': 74,
        'required_original_sha256': BASE, 'guard': 'COD4X1KHZ && KISAK_MP && COD4X1KHZ_IDENTITY_PROBE (off by default)',
        'root_owned_hook': 'guarded KHZ_InitIdentityProbe() at end of KHZ_InitFixedStep; handle failed initialization',
        'cases': total, 'category_counts': dict(kinds), 'terminated_prefix': 1,
        'guard_page_cases': 5, 'guard_page_cleanup': 1,
        'new_public_sdk_exports': 0, 'top_level_plugin_commands': len(registered_names),
        'top_level_plugin_command_names': registered_names,
        'parser_dispatch': 'khz_fixture_identity parse', 'guid_dispatch': 'khz_identity_guid_provider',
        'pending_harness_update': 'New native identity harness must use expected_subcommand=format|classify|guid|parse; do not alter archived74helpers.',
        'proposed_sha256': {relative: hashlib.sha256(text.encode()).hexdigest() for relative,text in new.items()}}
    (output / 'contract.json').write_text(json.dumps(contract, indent=2, sort_keys=True)+'\n', encoding='utf8')
    print(json.dumps(contract, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
