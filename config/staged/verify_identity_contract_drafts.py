"""Focused independent oracle checks, not execution of native SDK exports."""
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def pinned_bits(identifier, steam_only):
    # Literal pinned sapi.c masks, independent of the fixture's input fields.
    kind = (identifier & 0xF0000000000000) >> 52
    universe = (identifier & 0xFF00000000000000) >> 56
    instance = (identifier & 0xFFFFF00000000) >> 32
    allowed = universe == 1 if steam_only else universe == 1 or 32 <= universe < 40
    return allowed and kind == 1 and instance == 1


def main():
    fixture = (ROOT / 'sdk/tests/native_identity_contract_probe.inc').read_text()
    if not re.search(r'instances\[\]\s*=\s*\{0, 1, 2, 0xfffff\}', fixture):
        raise AssertionError('Reviewed instance input set changed')
    if not re.search(r'accounts\[\]\s*=\s*\{0, 0xffffffffU\}', fixture):
        raise AssertionError('Reviewed account input set changed')
    cases = 0
    for universe in range(256):
        for kind in range(16):
            for instance in (0, 1, 2, 0xfffff):
                for account in (0, 0xffffffff):
                    identifier = universe << 56 | kind << 52 | instance << 32 | account
                    eligible = kind == 1 and instance == 1
                    expected = (eligible and (universe == 1 or 32 <= universe <= 39),
                                eligible and universe == 1)
                    assert expected == (pinned_bits(identifier, False), pinned_bits(identifier, True))
                    cases += 1
    for bit in range(64):
        identifier = 76561197960278016 ^ (1 << bit)
        expected = (bit < 32 or bit == 61, bit < 32)
        assert expected == (pinned_bits(identifier, False), pinned_bits(identifier, True))
        cases += 1
    literals = (0, (1 << 64) - 1, 76561197960278073,
                2310346614158149229, 76561193665310777, 81064797587648569)
    for identifier, expected in zip(literals, ((False, False), (False, False), (True, True),
                                              (True, False), (False, False), (False, False))):
        assert expected == (pinned_bits(identifier, False), pinned_bits(identifier, True))
        cases += 1
    assert cases == 32838
    literal_body = re.search(r'literals\[\]\s*=\s*\{([^}]+)\}', fixture)[1]
    parsed = tuple((1 << 64) - 1 if value.strip() == 'UINT64_MAX'
                   else int(value.strip().removesuffix('ULL')) for value in literal_body.split(','))
    assert parsed == literals

    header = (ROOT / 'sdk/tests/identity_guid_goldens.inc').read_text()
    records = json.loads((ROOT / 'config/staged/identity_guid_goldens.json').read_text())['records']
    data = [(text, int(expected)) for text, expected in re.findall(r'\{"([0-9a-f]{32})", (\d+)ULL\}', header)]
    assert data == [(record['guid'], record['expected']) for record in records]
    assert len(data) == 7 and len(set(text for text, _ in data)) == 7
    assert 3 * len(data) + 32 * 5 + 64 + 2 == 247
    assert data[-1][1] == 2310346614158149229
    for text, identifier in data:
        assert len(bytes.fromhex(text)) == 16
        assert identifier >> 56 == 32
        assert (identifier >> 52) & 15 == 1
        assert (identifier >> 32) & 0xfffff == 1
    print(json.dumps({'passed': True, 'classifier_oracle_cases_per_export': cases,
                      'guid_literal_vectors': len(data), 'expected_guid_native_cases': 247,
                      'native_sdk_execution': False, 'semantic_proof': False}))


if __name__ == '__main__':
    main()
