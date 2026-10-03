"""Small pure source fixtures; no real census, binaries, games or hash sweep."""
from copy import deepcopy
from dataclasses import replace
import hashlib
import importlib.util
from pathlib import Path
import re
import sys
import unittest

NAME = 'staged_finite_enrollment'
module_path = Path(__file__).with_name('finite_registration_enrollment.py')
if '--production' in sys.argv:
    sys.argv.remove('--production')
    module_path = Path(__file__).resolve().parents[3] / 'scripts/finite_registration_enrollment.py'
SPEC = importlib.util.spec_from_file_location(NAME, module_path)
layer = importlib.util.module_from_spec(SPEC)
sys.modules[NAME] = layer
SPEC.loader.exec_module(layer)


def line(text, phrase):
    return text.count('\n', 0, text.index(phrase)) + 1


def field_asm(family, names):
    hud = family == 'hud'
    symbol = 'GScr_AddFieldsForHudElems' if hud else 'GScr_AddFieldsForEntity'
    table, width = ('fields', 7) if hud else ('ent_fields', 4)
    if hud:
        instructions = ['mov ebx, [fields]', 'mov esi, fields+0x1c',
            'movzx eax, ax', 'mov [esp+0x8], eax', 'mov [esp+0x4], ebx',
            'mov dword [esp], 0x1', 'call Scr_AddClassField',
            'add edi, 0x1c', 'add esi, 0x1c', 'test ebx, ebx',
            'jnz GScr_AddFieldsForHudElems_20', 'ret']
    else:
        instructions = ['mov edx, [ent_fields]', 'mov ebx, ent_fields+0x10',
            'sar eax, 0x4', 'movzx eax, ax', 'mov [esp+0x8], eax',
            'mov [esp+0x4], edx', 'mov dword [esp], 0x0', 'call Scr_AddClassField',
            'add esi, 0x10', 'add ebx, 0x10', 'test edx, edx',
            'jnz GScr_AddFieldsForEntity_20', 'jmp GScr_AddFieldsForClient']
    words = []
    for name in names:
        words += ['_cstring_' + name, *(['0x4', '0x1', '0x0', '0x0', 'Setter', 'Getter']
                                      if hud else ['0x10', '0x4', 'Setter'])]
    words += ['0x0'] * (11 if hud else 8)
    strings = '\n'.join('_cstring_' + name + ': db "' + name + '",0' for name in names)
    return symbol + ':\n' + '\n'.join('\t' + item for item in instructions) + '\n' + table + ': dd ' + ', '.join(words) + '\n' + strings + '\n'


def client_source():
    rows = '\n'.join('  { "' + name + '", 0, F_INT, NULL, NULL },' for name in layer.CLIENT)
    return ('#define ENTFIELD_MASK 0xc000\nstatic client_fields_s fields[] =\n{\n' + rows +
        '\n  { NULL, 0, F_INT, NULL, NULL }\n};\n'
        'void __cdecl GScr_AddFieldsForClient()\n{\n'
        '  for ( f = fields; f->name; ++f )\n  {\n'
        '    assert(!((f - fields) & ENTFIELD_MASK));\n'
        '    assert((f - fields) == (uint16_t)( f - fields ));\n'
        '    Scr_AddClassField(0, f->name, (uint16_t)(f - fields) | ENTFIELD_MASK);\n'
        '  }\n}\n')


def filter_source():
    result = ''
    for symbol, loop in (('Con_InitChannels', 'Con_InitChannels_50'),
                         ('Con_InitGameMsgChannels', 'Con_InitGameMsgChannels_10')):
        result += symbol + ':\n' + loop + ':\n' + '\n'.join('\t' + item for item in (
            'xor ebx, ebx', 'mov dword [ebp-0x20], defaultGameWindowFilters',
            'mov [esp+0x8], ebx', 'mov dword [esp+0x4], _cstring_con_gamemsgwindo',
            'call sprintf', 'mov [esp+0xc], edi', 'mov dword [esp+0x8], 0x29',
            'mov eax, [edx]', 'mov [esp+0x4], eax', 'mov [esp], esi',
            'call Cvar_RegisterString', 'add ebx, 0x1', 'cmp ebx, 0x4',
            'jnz ' + loop, 'ret')) + '\n'
    return (result + 'defaultGameWindowFilters: dd _cstring_first, _cstring_second, _cstring_third, _cstring_null, 0x0, 0x0, 0x0, 0x0\n'
        '_cstring_con_gamemsgwindo: db "con_gameMsgWindow%dFilter",0\n'
        '_cstring_first: db "gamenotify obituary",0\n_cstring_second: db "boldgame",0\n'
        '_cstring_third: db "subtitle",0\n_cstring_null: db 0\n')


def fixture(crlf=False, assembly_comments=False):
    texts = (field_asm('hud', layer.HUD), field_asm('entity', layer.ENTITY),
             client_source(), filter_source())
    reviews, sources = [], {}
    for base, text in zip(layer.REVIEWS, texts):
        if assembly_comments and base.family != 'client':
            text = text.replace('\tcall Scr_AddClassField', '\n\t; call is on the following actual line\n\tcall Scr_AddClassField')
            text = text.replace('\tcall Cvar_RegisterString', '\n\t; call is on the following actual line\n\tcall Cvar_RegisterString')
            table_name = {'hud': 'fields:', 'entity': 'ent_fields:',
                          'filters': 'defaultGameWindowFilters:'}[base.family]
            text = text.replace(table_name, ';Initialized global or static variables follow\n\nSECTION .data\n' + table_name, 1)
        if crlf:
            text = text.replace('\n', '\r\n')
        raw = text.encode()
        table = {'hud': 'fields:', 'entity': 'ent_fields:',
                 'client': 'static client_fields_s fields', 'filters': 'defaultGameWindowFilters:'}[base.family]
        api = 'Cvar_RegisterString' if base.family == 'filters' else 'Scr_AddClassField'
        calls = tuple(text.count('\n', 0, match.start()) + 1 for match in re.finditer(
            (r'^\tcall\s+' if base.family != 'client' else r'^    ') + api, text, re.M))
        review = replace(base, sha256=hashlib.sha256(raw).hexdigest(),
            table_line=line(text, table), call_lines=calls,
            format_line=line(text, '_cstring_con_gamemsgwindo:') if base.family == 'filters' else None)
        reviews.append(review)
        sources[review.path] = raw
    caps = {}
    for review in reviews:
        for index, identifier in enumerate(review.consumed()):
            kind = identifier.split(':', 2)[1]
            own = index < len(review.call_lines)
            caps[identifier] = {'id': identifier, 'domain': 'server', 'kind': kind,
                'name': identifier.split(':', 2)[2], 'resolved': False,
                'status': 'pending', 'applicable': True, 'provenance': [{
                    'path': review.path if own else layer.UPSTREAM_PATH + '/src/scr_vm_main.c',
                    'line': review.call_lines[index] if own else 783,
                    'sha256': review.sha256 if own else 'a' * 64,
                    'via': 'native registration' if own else 'registry initialization'}]}
        if review.family != 'filters':
            for name in review.names:
                identifier = review.id_prefix + name
                caps[identifier] = {'id': identifier, 'domain': 'server', 'kind': 'gsc_field',
                    'name': identifier.split(':', 2)[2], 'resolved': True,
                    'status': 'pending', 'applicable': True, 'existing_metadata': {'retain': name},
                    'provenance': [{'path': review.path, 'line': review.table_line,
                                    'sha256': review.sha256, 'via': 'named native field table'}]}
    for name, status in (('Plugin_Unrelated', 'proven'), ('Plugin_Another', 'pending'),
                         ('GScr_AddFieldsForRadiant', 'pending')):
        identifier = 'server:' + ('gsc_field_registry:' if name.startswith('GScr_') else 'sdk_export:') + name
        caps[identifier] = {'id': identifier, 'domain': 'server', 'kind': identifier.split(':')[1],
            'name': name, 'status': status, 'applicable': True, 'resolved': not name.startswith('GScr_'),
            'provenance': [{'path': 'untouched', 'line': 1, 'sha256': 'b' * 64}],
            'evidence': {'retain': True}}
    return caps, sources, tuple(reviews)


class FiniteEnrollment(unittest.TestCase):
    def resolve(self, caps, sources, reviews, revision=layer.REVISION):
        return layer._resolve(caps, lambda path: sources[path], revision, reviews)

    def refusal(self, caps, sources, reviews, revision=layer.REVISION):
        unchanged = deepcopy(caps)
        actual, errors = self.resolve(caps, sources, reviews, revision)
        self.assertTrue(errors)
        self.assertEqual(actual, unchanged)
        self.assertEqual(caps, unchanged)

    def changed_source(self, family, before, after, *, rebind_hash=True):
        caps, sources, reviews = fixture()
        index = next(index for index, review in enumerate(reviews) if review.family == family)
        review = reviews[index]
        original = sources[review.path].decode()
        self.assertIn(before, original)
        sources[review.path] = original.replace(before, after, 1).encode()
        if rebind_hash:
            replacement = replace(review, sha256=hashlib.sha256(sources[review.path]).hexdigest())
            reviews = (*reviews[:index], replacement, *reviews[index + 1:])
            # Treat the altered bytes as newly reviewed by the isolated fixture;
            # structural guards must still reject them beyond the hash guard.
            for entry in caps.values():
                for source in entry.get('provenance', []):
                    if source.get('path') == review.path:
                        source['sha256'] = replacement.sha256
        self.refusal(caps, sources, reviews)

    def test_exact_finite_diff_preserves_nonfamily_status_and_metadata(self):
        caps, sources, reviews = fixture()
        original = deepcopy(caps)
        actual, errors = self.resolve(caps, sources, reviews)
        self.assertEqual(errors, [])
        consumed = {name for review in reviews for name in review.consumed()}
        self.assertEqual(len(consumed), 7)
        self.assertEqual(set(caps) - set(actual), consumed)
        self.assertEqual(set(actual) - set(caps), {'server:dvar:' + name for name in layer.FILTERS})
        self.assertEqual(len(actual), len(caps) - 3)
        self.assertEqual(sum(item['status'] == 'proven' for item in actual.values()), 1)
        named = {review.id_prefix + name for review in reviews if review.family != 'filters' for name in review.names}
        self.assertEqual(len(named), 49)
        for identifier in set(caps) - consumed - named:
            self.assertEqual(actual[identifier], caps[identifier])
        for identifier in named:
            self.assertEqual(actual[identifier]['existing_metadata'], caps[identifier]['existing_metadata'])
            self.assertEqual(actual[identifier]['status'], 'pending')
            self.assertFalse(actual[identifier]['inventory_resolution']['semantic_proof'])
        self.assertEqual(caps, original)

    def test_class_offsets_and_entity_to_client_edge_survive(self):
        caps, sources, reviews = fixture()
        actual, errors = self.resolve(caps, sources, reviews)
        self.assertFalse(errors)
        for family, names, prefix, cls, mask in (
            ('hud', layer.HUD, 'server:gsc_field:_g_hudelem:', 1, 0),
            ('entity', layer.ENTITY, 'server:gsc_field:_g_spawn_mp:', 0, 0),
            ('client', layer.CLIENT, 'server:gsc_field:', 0, 0xc000)):
            for index, name in enumerate(names):
                resolved = actual[prefix + name]['inventory_resolution']
                self.assertEqual((resolved['class'], resolved['field_offset']), (cls, index | mask))
                if family == 'client':
                    self.assertEqual(resolved['registration_edge'], 'GScr_AddFieldsForEntity -> GScr_AddFieldsForClient')
                    self.assertTrue(any(item['via'] == 'registry initialization' for item in actual[prefix + name]['provenance']))
        for name in layer.FILTERS:
            entry = actual['server:dvar:' + name]
            self.assertEqual(entry['status'], 'pending')
            self.assertTrue(entry['applicable'])
            self.assertEqual(len(entry['provenance']), 2)

    def test_raw_aggregation_without_semantic_status_is_supported(self):
        caps, sources, reviews = fixture()
        for entry in caps.values():
            for key in ('status', 'applicable', 'reasons', 'evidence'):
                entry.pop(key, None)
        actual, errors = self.resolve(caps, sources, reviews)
        self.assertEqual(errors, [])
        for identifier in set(caps) & set(actual):
            self.assertNotIn('status', actual[identifier])
        for name in layer.FILTERS:
            self.assertEqual(actual['server:dvar:' + name]['status'], 'pending')

    def test_crlf_and_multiline_native_rows_are_supported(self):
        caps, sources, reviews = fixture(crlf=True)
        actual, errors = self.resolve(caps, sources, reviews)
        self.assertEqual(errors, [])
        self.assertEqual(len(actual), len(caps) - 3)

    def test_asm_comment_bounds_and_call_coordinates_are_exact(self):
        caps, sources, reviews = fixture(crlf=True, assembly_comments=True)
        actual, errors = self.resolve(caps, sources, reviews)
        self.assertEqual(errors, [])
        self.assertEqual(len(actual), len(caps) - 3)
        for review in reviews:
            rows = layer._validate_source(review, sources[review.path])
            self.assertEqual(tuple(row[0] for row in rows), review.names)
        quoted = '_cstring_literal: db "a;b",0 ; stripped comment\r\n'
        cleaned = layer._asm_comments(quoted)
        self.assertEqual(len(cleaned), len(quoted))
        self.assertEqual(cleaned.count('\n'), quoted.count('\n'))
        self.assertEqual(layer._asm_strings(cleaned), {'_cstring_literal': 'a;b'})

    def test_revision_and_missing_bytes_refuse_whole_transaction(self):
        caps, sources, reviews = fixture()
        self.refusal(caps, sources, reviews, revision='0' * 40)
        sources.pop(reviews[2].path)
        self.refusal(caps, sources, reviews)

    def test_hash_change_refuses_even_comment_only_modification(self):
        self.changed_source('hud', 'ret\n', 'ret\n; comment changed\n', rebind_hash=False)

    def test_strides_classes_mask_and_tail_call_refuse_beyond_hash_guard(self):
        for family, before, after in (
            ('hud', 'add edi, 0x1c', 'add edi, 0x18'),
            ('hud', 'mov dword [esp], 0x1', 'mov dword [esp], 0x0'),
            ('entity', 'sar eax, 0x4', 'sar eax, 0x3'),
            ('entity', 'jmp GScr_AddFieldsForClient', 'jmp GScr_AddFieldsForOther'),
            ('client', '#define ENTFIELD_MASK 0xc000', '#define ENTFIELD_MASK 0xb000'),
            ('client', 'Scr_AddClassField(0,', 'Scr_AddClassField(1,')):
            with self.subTest(family=family, mutation=after):
                self.changed_source(family, before, after)

    def test_format_bound_defaults_flags_and_coordinates_refuse(self):
        for before, after in (
            ('con_gameMsgWindow%dFilter', 'con_gameMsgWindow%dOther'),
            ('cmp ebx, 0x4', 'cmp ebx, 0x5'),
            ('mov dword [esp+0x8], 0x29', 'mov dword [esp+0x8], 0x21'),
            ('db "subtitle",0', 'db "other",0')):
            with self.subTest(mutation=after):
                self.changed_source('filters', before, after)
        caps, sources, reviews = fixture()
        for key, value in (('table_line', reviews[0].table_line + 1), ('call_lines', (999,))):
            changed = (replace(reviews[0], **{key: value}), *reviews[1:])
            self.refusal(caps, sources, changed)

    def test_missing_duplicate_extra_names_and_sentinels_refuse(self):
        for family, before, after in (
            ('hud', '_cstring_y: db "y",0', '_cstring_y: db "x",0'),
            ('entity', '_cstring_origin: db "origin",0', '_cstring_origin: db "missing",0'),
            ('client', '{ "name", 0, F_INT, NULL, NULL },', '{ "sessionteam", 0, F_INT, NULL, NULL },'),
            ('client', '{ NULL, 0, F_INT, NULL, NULL }', '{ NULL, 1, F_INT, NULL, NULL }'),
            ('hud', '0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0\n_cstring_x:', '0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0\n_cstring_x:')):
            with self.subTest(family=family, mutation=after):
                self.changed_source(family, before, after)

    def test_missing_alias_wrong_binding_and_status_cannot_erase_requirements(self):
        for mutate in ('missing', 'kind', 'source_line', 'proven', 'excluded'):
            caps, sources, reviews = fixture()
            identifier = reviews[0].consumed()[0]
            if mutate == 'missing':
                caps.pop(identifier)
            elif mutate == 'kind':
                caps[identifier]['kind'] = 'command'
            elif mutate == 'source_line':
                caps[identifier]['provenance'][0]['line'] += 1
            elif mutate == 'proven':
                caps[identifier]['status'] = 'proven'
            else:
                caps[identifier]['status'] = 'excluded'
                caps[identifier]['applicable'] = False
            with self.subTest(mutate=mutate):
                self.refusal(caps, sources, reviews)
        caps, sources, reviews = fixture()
        caps['server:dvar:' + layer.FILTERS[0]] = {'id': 'duplicate'}
        self.refusal(caps, sources, reviews)

    def test_public_entrypoint_has_no_freeform_status_or_consumption_override(self):
        with self.assertRaises(TypeError):
            replace(layer.REVIEWS[0], status='proven')
        caps, sources, reviews = fixture()
        actual, errors = layer.enroll_finite_registrations(caps, Path('.'),
            {'path': '../elsewhere', 'revision': layer.REVISION})
        self.assertEqual(actual, caps)
        self.assertTrue(errors)


if __name__ == '__main__':
    unittest.main()
