"""Independent expected values for the proposed native parser batch.

These are case data and layout checks, not an implementation of the parser.
No production helper is imported and no semantic evidence is generated here.
The native implementation/fixture remain unchanged until source review.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class Case:
    label: str
    text: str | None
    expected: int
    contract: str


ALIASES = (('U', 1), ('M', 2), ('G', 3), ('A', 4), ('P', 5), ('C', 6),
           ('g', 7), ('T', 8), ('c', 8), ('L', 8), ('a', 10))
UNIVERSES = (1, 2, 3, 4, 32, 33, 34, 35, 36, 37, 38, 39)
ASCII_WHITESPACE = (' ', '\t', '\r', '\n', '\v', '\f')


def bits(universe, account_type, instance, account):
    """Assemble the documented layout directly, without textual conversion."""
    if not (0 <= universe < 256 and 0 <= account_type < 16 and
            0 <= instance < 0x100000 and 0 <= account < 0x100000000):
        raise ValueError('Expected-value bit field overflow')
    return (universe << 56) | (account_type << 52) | (instance << 32) | account


PINNED_VALID = (
    Case('legacy-instance0-even', 'STEAM_0:0:0', 0x0110000000000000, 'pinned-valid'),
    Case('legacy-instance1-odd', 'STEAM_1:1:21', 0x011000010000002b, 'pinned-valid'),
    Case('legacy-zero-padding', 'STEAM_01:01:00021', 0x011000010000002b, 'pinned-valid'),
    Case('legacy-positive-fields', 'STEAM_ +1:+1: +21', 0x011000010000002b, 'pinned-valid'),
    Case('legacy-outer-spaces', '  STEAM_1:1:21  ', 0x011000010000002b, 'pinned-valid'),
    Case('steam3-default-individual', 'U:1:42', 0x011000010000002a, 'pinned-valid'),
    Case('steam3-brackets', '[U:1:42]', 0x011000010000002a, 'pinned-valid'),
    Case('steam3-outer-spaces', ' [U:1:42] ', 0x011000010000002a, 'pinned-valid'),
    Case('steam3-positive-fields', '[U: +1: +42]', 0x011000010000002a, 'pinned-valid'),
    Case('steam3-private-universe', '[U:32:42]', 0x201000010000002a, 'pinned-valid'),
    Case('steam3-account-maximum', 'U:1:4294967295', 0x01100001ffffffff, 'pinned-valid'),
    Case('legacy-high-maximum', 'STEAM_1:1:2147483647', 0x01100001ffffffff, 'pinned-valid'),
    Case('decimal-public-golden', '76561197960265770', 0x011000010000002a, 'pinned-valid'),
    Case('decimal-outer-spaces', ' 76561197960265770 ', 0x011000010000002a, 'pinned-valid'),
    Case('decimal-zero-padding', '00076561197960265770', 0x011000010000002a, 'pinned-valid'),
)

BUG_CORRECTIONS = (
    Case('steam3-explicit-zero', '[U:1:42:0]', 0x011000000000002a, 'bug-correction-four-fields'),
    Case('steam3-explicit-one', 'U:1:42:1', 0x011000010000002a, 'bug-correction-four-fields'),
    Case('steam3-explicit-max', '[U:1:42:1048575]', 0x011fffff0000002a, 'bug-correction-four-fields'),
    Case('steam3-explicit-leading-plus', '[U:1:42: +1]', 0x011000010000002a, 'bug-correction-four-fields'),
)

REFUSALS = tuple(Case(label, text, 0, contract) for label, text, contract in (
    ('null', None, 'safety-null'), ('empty', '', 'malformed'),
    ('spaces-only', '    ', 'malformed'), ('unknown-type', 'I:1:42', 'malformed'),
    ('type-case', 'u:1:42', 'malformed'), ('type-leading-tab', '\tU:1:42', 'malformed'),
    ('type-extra-byte', 'UU:1:42', 'malformed'),
    ('legacy-case', 'steam_1:1:21', 'malformed'),
    ('legacy-negative-instance', 'STEAM_-1:1:21', 'safety-negative'),
    ('legacy-negative-low', 'STEAM_1:-1:21', 'safety-negative'),
    ('legacy-negative-high', 'STEAM_1:1:-21', 'safety-negative'),
    ('legacy-instance-overflow', 'STEAM_65537:1:21', 'safety-overflow'),
    ('legacy-low-overflow', 'STEAM_1:2:21', 'safety-low-bit-bound'),
    ('legacy-high-overflow', 'STEAM_1:1:2147483648', 'safety-shift-overflow'),
    ('legacy-u32-overflow', 'STEAM_1:1:4294967296', 'safety-overflow'),
    ('universe-zero', 'U:0:42', 'malformed'), ('universe-gap', 'U:31:42', 'malformed'),
    ('universe-upper', 'U:40:42', 'malformed'), ('universe-overflow', 'U:256:42', 'safety-overflow'),
    ('negative-universe', 'U:-1:42', 'safety-negative'),
    ('negative-account', 'U:1:-42', 'safety-negative'),
    ('account-overflow', 'U:1:4294967296', 'safety-overflow'),
    ('negative-instance', '[U:1:42:-1]', 'safety-negative'),
    ('instance-overflow', '[U:1:42:1048576]', 'safety-overflow'),
    ('plus-without-digits', 'U:+:42', 'malformed'),
    ('double-plus', 'U:++1:42', 'malformed'),
    ('space-before-colon', 'U:1 :42', 'malformed'),
    ('space-inside-bracket', '[U:1:42 ]', 'malformed'),
    ('opening-bracket-only', '[U:1:42', 'malformed'),
    ('closing-bracket-only', 'U:1:42]', 'malformed'),
    ('nested-brackets', '[[U:1:42]]', 'malformed'),
    ('extra-field', 'U:1:42:1:0', 'malformed'), ('empty-field', 'U::42', 'malformed'),
    ('trailing-colon', 'U:1:42:', 'malformed'),
    ('trailing-junk', 'U:1:42garbage', 'safety-junk'),
    ('decimal-plus', '+76561197960265770', 'malformed'),
    ('decimal-negative', '-76561197960265770', 'safety-negative'),
    ('decimal-overflow', '18446744073709551616', 'safety-overflow'),
    ('decimal-prefix-junk', '76561197960265770 garbage', 'safety-junk'),
    ('decimal-newline-junk', '76561197960265770\njunk', 'safety-junk'),
    ('decimal-bracket-extension', '[76561197960265770]', 'removed-extension'),
    ('128byte-truncation', '76561197960265770' + ' ' * 111, 'safety-length'),
))


def native_cases():
    """Finite source cases for a future generated native C fixture only."""
    result = list(PINNED_VALID + BUG_CORRECTIONS + REFUSALS)
    for alias, account_type in ALIASES:
        for universe in UNIVERSES:
            for account in (0, 42, 0xffffffff):
                expected = bits(universe, account_type, 1 if alias in ('U', 'G') else 0, account)
                for brackets in (False, True):
                    text = f'{alias}:{universe}:{account}'
                    result.append(Case(f'type-universe-{ord(alias)}-{universe}-{account}-{int(brackets)}',
                                       '[' + text + ']' if brackets else text, expected, 'pinned-valid'))
                    for instance in (0, 1, 0xfffff):
                        explicit = text + ':' + str(instance)
                        result.append(Case(f'explicit-{ord(alias)}-{universe}-{account}-{instance}-{int(brackets)}',
                            '[' + explicit + ']' if brackets else explicit,
                            bits(universe, account_type, instance, account), 'bug-correction-four-fields'))
    for index, whitespace in enumerate(ASCII_WHITESPACE):
        result.append(Case(f'legacy-leading-whitespace-{index}',
            'STEAM_' + whitespace + '+1:' + whitespace + '+1:' + whitespace + '+21',
            0x011000010000002b, 'pinned-valid'))
        result.append(Case(f'steam3-leading-whitespace-{index}',
            '[U:' + whitespace + '+1:' + whitespace + '+42]',
            0x011000010000002a, 'pinned-valid'))
    # A full universe-byte refusal matrix is generated directly from the
    # reviewed set rather than copied from the production validId helper.
    for universe in range(256):
        if universe not in UNIVERSES:
            result.append(Case(f'invalid-universe-{universe}', f'U:{universe}:42', 0, 'malformed'))
        for account_type in range(16):
            value = bits(universe, account_type, 1, 42)
            valid = universe in UNIVERSES and 1 <= account_type <= 10
            result.append(Case(f'decimal-bits-{universe}-{account_type}', str(value),
                               value if valid else 0, 'pinned-valid' if valid else 'malformed'))
    for instance in (0, 1):
        for low in (0, 1):
            for high in (0, 1, 0x7fffffff):
                result.append(Case(f'legacy-fields-{instance}-{low}-{high}',
                    f'STEAM_{instance}:{low}:{high}', bits(1, 1, instance, high * 2 + low), 'pinned-valid'))
    for universe in UNIVERSES:
        for account_type in range(1, 11):
            for instance in (0, 0xfffff):
                for account in (0, 0xffffffff):
                    value = bits(universe, account_type, instance, account)
                    result.append(Case(f'decimal-corner-{universe}-{account_type}-{instance}-{account}',
                                       str(value), value, 'pinned-valid'))
    result.append(Case('127byte-outer-spaces', '76561197960265770' + ' ' * 110,
                       0x011000010000002a, 'pinned-valid'))
    return tuple(result)

