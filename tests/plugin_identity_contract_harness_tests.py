"""Truthful strict marker parsing; no native executable or process launch."""
import importlib.util
from pathlib import Path
import os
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('identity_contract_fixture', ROOT / 'scripts/plugin_identity_contract_native_probe.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def line(name, fields):
    return name + ''.join(f' {key}={value}' for key, value in fields.items()) + '\n'


class IdentityContractMarkers(unittest.TestCase):
    def test_exact_classifier_and_guid_markers(self):
        source = line('KHZ_IDENTITY_INDIVIDUAL', fixture.CLASSIFIER_FIELDS) + line('KHZ_IDENTITY_STEAM_ACCOUNT', fixture.CLASSIFIER_FIELDS)
        self.assertEqual(len(fixture.classified(source)), 2)
        self.assertEqual(fixture.converted_guid(line('KHZ_IDENTITY_GUID', fixture.GUID_FIELDS))['fields']['cases'], 247)

    def test_missing_duplicate_and_noncanonical_markers_reject(self):
        for source in ('', 'KHZ_IDENTITY_GUID cases=247\nKHZ_IDENTITY_GUID cases=247\n',
                       'KHZ_IDENTITY_GUID cases=247 cases=247\n', 'KHZ_IDENTITY_GUID cases=-247\n'):
            with self.assertRaises(ValueError):
                fixture.converted_guid(source)

    def test_wrong_case_counts_failure_and_provider_fields_reject(self):
        for key, value in (('cases', 246), ('failures', 1), ('goldens', 6),
                           ('provider_authentication', 1), ('provider_failure_injected', 1)):
            changed = dict(fixture.GUID_FIELDS, **{key: value})
            with self.assertRaises(ValueError):
                fixture.converted_guid(line('KHZ_IDENTITY_GUID', changed))

    def test_classifier_missing_second_export_and_partial_matrix_reject(self):
        with self.assertRaises(ValueError):
            fixture.classified(line('KHZ_IDENTITY_INDIVIDUAL', fixture.CLASSIFIER_FIELDS))
        changed = dict(fixture.CLASSIFIER_FIELDS, matrix=32767)
        with self.assertRaises(ValueError):
            fixture.classified(line('KHZ_IDENTITY_INDIVIDUAL', changed) + line('KHZ_IDENTITY_STEAM_ACCOUNT', fixture.CLASSIFIER_FIELDS))

    def test_detail_failure_cannot_hide_under_success_counts(self):
        good = line('KHZ_IDENTITY_GUID', fixture.GUID_FIELDS)
        with self.assertRaises(ValueError):
            fixture.converted_guid(good + 'KHZ_IDENTITY_CONTRACT_FAILURE actual_case\n')

    def test_dispatch_rejection_cannot_execute_a_probe(self):
        refusal = 'KHZ_IDENTITY_DISPATCH rejected=1 expected_subcommand=format|classify|guid\n'
        self.assertTrue(fixture.rejected(refusal))
        self.assertFalse(fixture.rejected(refusal * 2))
        for detail in ('KHZ_IDENTITY_GUID', 'KHZ_PLUGIN_IDENTITY_FIXTURE', 'KHZ_IDENTITY_FORMAT_SELECTED'):
            self.assertFalse(fixture.rejected(refusal + detail + '\n'))


class IdentityContractEnrollment(unittest.TestCase):
    def test_ordinary_file_and_fresh_direct_home(self):
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary).resolve()
            runtime = project / 'runtime'
            runtime.mkdir()
            document = project / 'oracle.json'
            document.write_text('{}')
            with patch.object(fixture, 'PROJECT', project), patch.object(fixture, 'RUNTIME', runtime):
                self.assertEqual(fixture.project_file('oracle.json'), document)
                self.assertEqual(fixture.fresh_home(28986), runtime / 'benchmarks/server-28986')
                (runtime / 'benchmarks').mkdir()
                (runtime / 'benchmarks/server-28986').mkdir()
                with self.assertRaises(ValueError):
                    fixture.fresh_home(28986)

    def test_lexical_parent_traversal_and_outside_file_reject_before_open(self):
        for name in ('docs/../docs/public-capabilities.json', '../cod4x1khz/docs/public-capabilities.json'):
            with self.assertRaises(ValueError):
                fixture.project_file(name)
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                fixture.project_file(Path(temporary) / 'outside.json')

    def test_runtime_reparse_ancestor_refuses_home_even_if_resolved_path_is_inside(self):
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary).resolve()
            runtime = project / 'runtime'
            runtime.mkdir()
            original = Path.lstat

            def attributes(path):
                if path == runtime:
                    return SimpleNamespace(st_mode=stat.S_IFDIR,
                        st_file_attributes=getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))
                return original(path)

            with patch.object(fixture, 'PROJECT', project), patch.object(fixture, 'RUNTIME', runtime), patch.object(Path, 'lstat', attributes):
                with self.assertRaises(ValueError):
                    fixture.fresh_home(28986)

    def test_actual_runtime_symlink_escaping_project_refuses_home(self):
        with tempfile.TemporaryDirectory() as temporary, tempfile.TemporaryDirectory() as outside:
            project = Path(temporary).resolve()
            runtime = project / 'runtime'
            try:
                os.symlink(outside, runtime, target_is_directory=True)
            except (OSError, NotImplementedError) as error:
                self.skipTest('Host cannot create this test symlink: ' + str(error))
            with patch.object(fixture, 'PROJECT', project), patch.object(fixture, 'RUNTIME', runtime):
                with self.assertRaises(ValueError):
                    fixture.fresh_home(28986)

    def test_exact_seven_unique_independently_passed_goldens(self):
        expected = [{'guid': f'{index:032x}', 'expected': index} for index in range(7)]
        records = [{'guid': row['guid'], 'actual': row['expected'], 'passed': True} for row in expected]
        fixture.check_goldens({'passed': True, 'vectors': 7, 'records': records}, {'records': expected})
        for changed in (records[:-1], records + [records[0]],
                        [dict(records[0], passed=False), *records[1:]],
                        [dict(records[0], guid=[]), *records[1:]],
                        [dict(records[0], actual=True), *records[1:]],
                        [records[0], records[0], *records[2:]]):
            with self.assertRaises(ValueError):
                fixture.check_goldens({'passed': True, 'vectors': 7, 'records': changed}, {'records': expected})
        with self.assertRaises(ValueError):
            fixture.check_goldens({'passed': True, 'vectors': 7, 'records': records}, {'records': expected[:-1]})


if __name__ == '__main__':
    unittest.main()
