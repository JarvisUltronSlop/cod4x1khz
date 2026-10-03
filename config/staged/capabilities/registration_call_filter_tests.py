"""Focused staged tests; import only, no census write/hash/game/build."""
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


census = module('existing_capability_scanner', ROOT / 'scripts/capabilities.py')
candidate = module('staged_registration_filter', Path(__file__).with_name('registration_call_filter.py'))


def registrations(source):
    clean = census.inactive_zero(census.strip_comments(source))
    masked = census.mask_literals(clean)
    observed = []
    for match in census.CALL_RE.finditer(masked):
        body, end = census.balanced(clean, match.end() - 1)
        args = census.arguments(body)
        if not candidate.registration_is_declaration(masked, match.start(), end, args):
            observed.append((match.group(1), args))
    return observed


class RegistrationCallFilter(unittest.TestCase):
    def test_exact_six_manifest_prototypes_do_not_become_dynamic_dvars(self):
        declarations = '\n'.join('void ' + name + '(void);' for name in
            ('Cvar_RegisterBool_f', 'Cvar_RegisterInt_f', 'Cvar_RegisterFloat_f'))
        self.assertEqual(registrations(declarations * 2), [])

    def test_multiline_unknown_return_pointer_calling_convention_and_attributes(self):
        sources = (
            'extern\nconst dvar_t *\n__cdecl\nCvar_RegisterCustom(\ncustom_name_t name, custom_value_t value\n);',
            '__declspec(dllimport) dvar_t * WINAPI Cvar_RegisterCustom(custom_name_t);',
            '__attribute__((visibility("default"))) const struct dvar_s *\nCvar_RegisterCustom(custom_name_t) __attribute__((nonnull(1)));',
            'extern "C" { dvar_t * __stdcall Cvar_RegisterCustom(); }',
            'namespace registry { internal::dvar_t * Cvar_RegisterCustom(custom_name_t); }',
            'dvar_t * Cvar_RegisterCustom(const char *name) { return 0; }',
        )
        for source in sources:
            with self.subTest(source=source):
                self.assertEqual(registrations(source), [])

    def test_assignments_and_same_name_calls_survive_prototypes(self):
        source = '''
extern const dvar_t * Cvar_RegisterBool(const char *, bool, int, const char *);
void Cvar_RegisterBool_f(void);
void use(void) {
    dvar_t * result = Cvar_RegisterBool("actual", 1, 0, "actual description");
    Cvar_RegisterBool_f();
    result = Cvar_RegisterBool(name, flag, flags, description);
}
'''
        actual = registrations(source)
        self.assertEqual([name for name, args in actual],
            ['Cvar_RegisterBool', 'Cvar_RegisterBool_f', 'Cvar_RegisterBool'])
        self.assertEqual(actual[0][1][0], '"actual"')
        self.assertEqual(actual[2][1][0], 'name')

    def test_expression_context_casts_nested_calls_return_and_conditions_survive(self):
        source = '''
void use(void) {
  return Cvar_RegisterString(name, value, flags, description);
  if (Cvar_RegisterBool("condition", value, flags, description)) { }
  callback(Cvar_RegisterInt(name, value, -1, 1, flags, description));
  output = (dvar_t*)Cvar_RegisterCustom((const char*)name, value);
}
'''
        self.assertEqual([name for name, args in registrations(source)],
            ['Cvar_RegisterString', 'Cvar_RegisterBool', 'Cvar_RegisterInt', 'Cvar_RegisterCustom'])

    def test_registry_declarations_are_distinct_from_actual_field_calls(self):
        source = '''
void GScr_AddFieldsForEntity(void);
void Scr_AddClassField(unsigned int classnum, const char *name, unsigned short offset);
void use(void) {
  GScr_AddFieldsForEntity();
  Scr_AddClassField(0, row->name, index | 0xc000);
}
'''
        self.assertEqual([name for name, args in registrations(source)],
            ['GScr_AddFieldsForEntity', 'Scr_AddClassField'])

    def test_comments_strings_inactive_calls_do_not_appear(self):
        source = '''
/* Cvar_RegisterBool("comment", 0, 0, ""); */
const char *sample = "Cvar_RegisterBool(\\\"text\\\", 0, 0, \\\"\\\");";
#if 0
Cvar_RegisterBool("inactive", 0, 0, "");
#endif
void use(void) { Cvar_RegisterBool("actual", 0, 0, ""); }
'''
        self.assertEqual(len(registrations(source)), 1)
        self.assertEqual(registrations(source)[0][1][0], '"actual"')

    def test_unknown_function_like_macro_is_kept_for_review(self):
        self.assertEqual(len(registrations('#define REGISTER(name) Cvar_RegisterCustom(name, value)\n')), 1)


if __name__ == '__main__':
    unittest.main()
