"""Pure staging contract checks; no original-source hashes or native processes."""
from unittest.mock import patch
import unittest
import prepare_identity_proposal as proposal
from parser_contract_cases import Case, native_cases


class ProposalData(unittest.TestCase):
    def test_c_literals_preserve_byte_boundaries(self):
        self.assertEqual(proposal.c_string(None), 'NULL')
        self.assertEqual(proposal.c_string(''), '""')
        self.assertEqual(proposal.c_string('"\\\n\t\0' + '7'), '"\\"\\\\\\012\\011\\0007"')
        with self.assertRaises(ValueError):
            proposal.c_string('\u00a0')

    def test_anchor_refuses_missing_or_ambiguous_original(self):
        self.assertEqual(proposal.once('abc', 'b', 'x'), 'axc')
        for source in ('abc', 'bb'):
            with self.assertRaises(ValueError):
                proposal.once(source, 'bb' if source == 'abc' else 'b', 'replacement')

    def test_generated_native_counts_and_literal_cases_are_explicit(self):
        generated, kinds, count = proposal.native_fixture()
        cases = native_cases()
        self.assertEqual(count, len(cases) + 7)
        self.assertEqual(sum(kinds.values()), len(cases))
        self.assertEqual(generated.count('    {"'), len(cases))
        self.assertIn('Plugin_StringToSteamID(NULL)', generated)
        self.assertIn('Plugin_StringToSteamID(terminated)', generated)
        self.assertIn('!memcmp(storage, original, sizeof(storage))', generated)
        self.assertIn('0x011000010000002aULL', generated)
        self.assertIn('expected', generated)
        self.assertIn('provider_authentication=0', generated)
        self.assertIn('guard_page_cases=5', generated)
        self.assertIn('guard_128_nonterminated_bytes', generated)
        self.assertIn('guard_127_length_terminated', generated)
        self.assertIn('guard_128_length_terminated', generated)
        for value in range(5):
            self.assertGreater(kinds[value], 0)

    def test_duplicate_native_case_labels_refuse(self):
        sample = Case('same', 'U:1:42', 0x011000010000002a, 'pinned-valid')
        with patch.object(proposal, 'native_cases', return_value=(sample, sample)):
            with self.assertRaises(ValueError):
                proposal.native_fixture()

    def test_fixture_command_capacity_is_checked_before_preparation(self):
        text = '\n'.join('Plugin_AddCommand("command' + str(index) + '",callback,0);' for index in range(20))
        self.assertEqual(len(proposal.command_names(text)), 20)
        for altered in (text + '\nPlugin_AddCommand("extra",callback,0);',
                        text.replace('command19', 'command18'),
                        text.replace('Plugin_AddCommand("command19",callback,0);', '')):
            with self.assertRaises(ValueError):
                proposal.command_names(altered)


if __name__ == '__main__':
    unittest.main()
