"""Prevent fixture-only growth from hiding real public SDK probes."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fixture_archive', ROOT / 'scripts/plugin_fixture_archive.py')
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)
SOURCE = (ROOT / 'sdk/tests/regression_plugin.c').read_text()
NATIVE = (ROOT / 'native/plugin_registry.hpp').read_text()
PINNED = (ROOT / 'upstream/cod4x_server/src/plugin_handler.h').read_text()


class FixtureRegistration(unittest.TestCase):
    def check_inventory(self, source=SOURCE, native=NATIVE, pinned=PINNED):
        return archive.inspect_fixture_registration(source, native, pinned)

    def test_real_archived_shape_keeps_final_udp(self):
        result = self.check_inventory()
        self.assertEqual(result['observed_count'], 20)
        self.assertEqual(result['commands'][-1]['name'], 'khz_fixture_udp')

    def test_actual_21st_registration_failure_is_caught(self):
        source = SOURCE.replace('    Plugin_AddCommand("khz_fixture_udp",',
            '    Plugin_AddCommand("khz_fixture_extra", identity_dispatch, 100);\n    Plugin_AddCommand("khz_fixture_udp",')
        with self.assertRaisesRegex(ValueError, 'registers 21'):
            self.check_inventory(source)

    def test_duplicate_and_missing_udp_are_caught(self):
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            self.check_inventory(SOURCE.replace('Plugin_AddCommand("khz_fixture_udp",', 'Plugin_AddCommand("khz_fixture_identity",'))
        with self.assertRaisesRegex(ValueError, 'missing'):
            self.check_inventory(SOURCE.replace('Plugin_AddCommand("khz_fixture_udp",', 'Plugin_AddCommand("khz_fixture_other",'))

    def test_comments_and_literals_do_not_create_registrations(self):
        source = SOURCE.replace('PCL int OnInit(void) {',
            'PCL int OnInit(void) {\n /* Plugin_AddCommand("fake", identity_dispatch, 100); } */\n'
            ' // Plugin_AddCommand("fake2", identity_dispatch, 100); {\n'
            ' const char* ignored = "Plugin_AddCommand(\\\"fake3\\\", identity_dispatch, 100); }";\n')
        self.assertEqual(self.check_inventory(source)['observed_count'], 20)

    def test_conditional_or_nonliteral_registration_requires_review(self):
        with self.assertRaisesRegex(ValueError, 'unconditional'):
            self.check_inventory(SOURCE.replace('Plugin_AddCommand("khz_fixture_udp", udp_fixture_command, 100);',
                'if (1) { Plugin_AddCommand("khz_fixture_udp", udp_fixture_command, 100); }'))
        with self.assertRaisesRegex(ValueError, 'literal'):
            self.check_inventory(SOURCE.replace('Plugin_AddCommand("khz_fixture_udp",', 'Plugin_AddCommand(dynamic_name,'))

    def test_capacity_drift_is_caught(self):
        with self.assertRaisesRegex(ValueError, 'capacity'):
            self.check_inventory(native=NATIVE.replace('power, 20, slot)', 'power, 21, slot)'))
        with self.assertRaisesRegex(ValueError, 'capacity'):
            self.check_inventory(pinned=PINNED.replace('pluginCmd_t cmd[20]', 'pluginCmd_t cmd[21]'))


if __name__ == '__main__':
    unittest.main()
