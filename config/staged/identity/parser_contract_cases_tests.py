"""Case-data correctness checks; these never execute the production parser."""
import unittest
from parser_contract_cases import (ALIASES, ASCII_WHITESPACE, BUG_CORRECTIONS,
                                   PINNED_VALID, REFUSALS, UNIVERSES, bits, native_cases)


class ParserCaseData(unittest.TestCase):
    def test_literal_bit_goldens_are_independent(self):
        self.assertEqual(bits(1, 1, 1, 42), 0x011000010000002a)
        self.assertEqual(bits(32, 1, 1, 42), 0x201000010000002a)
        self.assertEqual(bits(1, 1, 0xfffff, 42), 0x011fffff0000002a)
        self.assertEqual(int('76561197960265770'), bits(1, 1, 1, 42))
        for position, too_large in enumerate((256, 16, 0x100000, 0x100000000)):
            fields = [1, 1, 1, 1]
            fields[position] = too_large
            with self.assertRaises(ValueError):
                bits(*fields)

    def test_complete_alias_universe_and_whitespace_enrollment(self):
        self.assertEqual(len(ALIASES), 11)
        self.assertEqual(len(UNIVERSES), 12)
        self.assertEqual(len(ASCII_WHITESPACE), 6)
        self.assertEqual(dict(ALIASES)['c'], 8)
        self.assertEqual(dict(ALIASES)['L'], 8)
        cases = native_cases()
        self.assertEqual(len({case.label for case in cases}), len(cases))
        self.assertEqual(sum(case.label.startswith('type-universe-') for case in cases), 792)
        self.assertEqual(sum(case.label.startswith('explicit-') for case in cases), 2376)
        self.assertEqual(sum(case.label.startswith('invalid-universe-') for case in cases), 244)
        self.assertEqual(sum(case.label.startswith('decimal-bits-') for case in cases), 4096)
        self.assertEqual(sum(case.label.startswith('decimal-corner-') for case in cases), 480)
        self.assertEqual(sum('leading-whitespace-' in case.label for case in cases), 12)

    def test_error_corrections_do_not_get_labeled_pinned_valid(self):
        self.assertTrue(all(case.expected and case.contract == 'pinned-valid' for case in PINNED_VALID))
        self.assertTrue(all(case.expected and case.contract == 'bug-correction-four-fields' for case in BUG_CORRECTIONS))
        self.assertTrue(all(case.expected == 0 and case.contract != 'pinned-valid' for case in REFUSALS))
        long_case = next(case for case in REFUSALS if case.label == '128byte-truncation')
        self.assertEqual(len(long_case.text), 128)
        self.assertEqual(next(case for case in REFUSALS if case.label == 'decimal-bracket-extension').contract,
                         'removed-extension')


if __name__ == '__main__':
    unittest.main()
