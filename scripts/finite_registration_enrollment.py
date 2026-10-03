"""Reviewed source-identity enrollment, never semantic evidence or exclusion.

Production integration must call enroll_finite_registrations after raw source
aggregation and before evidence validation. The public entrypoint accepts no
free-form resolution/status records: it uses these exact reviewed pinned specs.
Every family is validated before any entry is changed; failure preserves all
original opaque requirements. _resolve is dependency-injected for tiny tests.
"""
from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re

REVISION = 'ddd65fce1a1093017bad2dbb6840d75d11c0d56f'
UPSTREAM_PATH = 'upstream/cod4x_server'
UPSTREAM_URL = 'https://github.com/callofduty4x/cod4x_server'
HUD = ('x', 'y', 'z', 'fontscale', 'font', 'alignx', 'aligny', 'horzalign',
       'vertalign', 'color', 'alpha', 'label', 'sort', 'foreground',
       'hidewhendead', 'hidewheninmenu', 'glowcolor', 'glowalpha', 'archived')
ENTITY = ('classname', 'origin', 'model', 'spawnflags', 'target', 'targetname',
          'count', 'health', 'dmg', 'angles')
CLIENT = ('sessionteam', 'name', 'maxhealth', 'headiconteam', 'kills', 'deaths',
          'assists', 'killcamentity', 'score', 'sessionstate', 'statusicon',
          'spectatorclient', 'archivetime', 'psoffsettime', 'pers', 'headicon',
          'groundentity', 'hasradar', 'isbot', 'localization')
FILTERS = tuple(f'con_gameMsgWindow{index}Filter' for index in range(4))
DEFAULTS = ('gamenotify obituary', 'boldgame', 'subtitle', '')


@dataclass(frozen=True)
class Review:
    family: str
    path: str
    sha256: str
    table_line: int
    call_lines: tuple[int, ...]
    names: tuple[str, ...]
    id_prefix: str
    registry_id: str | None = None
    format_line: int | None = None
    null_tail_words: int | None = None

    def consumed(self):
        kind = 'dvar' if self.family == 'filters' else 'gsc_field'
        dynamic = tuple(f'server:{kind}:dynamic@{self.path}:{line}' for line in self.call_lines)
        return dynamic + ((self.registry_id,) if self.registry_id else ())


REVIEWS = (
    Review('hud', UPSTREAM_PATH + '/src/asmsource/_g_hudelem.asm',
           '9ba8cc2682225f9c1ee38d6a05c56a0295e7c470fb2cd85d7550cae77658a11f',
           2536, (2469,), HUD, 'server:gsc_field:_g_hudelem:',
           'server:gsc_field_registry:GScr_AddFieldsForHudElems', null_tail_words=11),
    Review('entity', UPSTREAM_PATH + '/src/asmsource/_g_spawn_mp.asm',
           '9f365ea26b1ef5c08bdbaea8c1f9a352f07f5b050855fb00b92bd38b2f27c38d',
           1649, (1628,), ENTITY, 'server:gsc_field:_g_spawn_mp:',
           'server:gsc_field_registry:GScr_AddFieldsForEntity', null_tail_words=8),
    Review('client', UPSTREAM_PATH + '/src/g_client_fields.cpp',
           '4ade79eb14277621bb45275e3b81953262df50837b3bf0a252bea64e4cd74382',
           422, (460,), CLIENT, 'server:gsc_field:'),
    Review('filters', UPSTREAM_PATH + '/src/asmsource/_con_channels.asm',
           '1ea388e11dd2709f4fbefc7ee66af9fc2f8b4622ec2312a395424bc9e09f0c66',
           842, (535, 733), FILTERS, 'server:dvar:', format_line=876),
)


def _line(text, offset):
    return text.count('\n', 0, offset) + 1


def _only(pattern, text, flags=re.M):
    found = list(re.finditer(pattern, text, flags))
    if len(found) != 1:
        raise ValueError('Expected one source binding: ' + pattern)
    return found[0]


def _balanced(text, start, left='{', right='}'):
    depth, quote, cursor = 0, None, start
    while cursor < len(text):
        char = text[cursor]
        if quote:
            if char == '\\':
                cursor += 2
                continue
            if char == quote:
                quote = None
        elif char in '\"\'':
            quote = char
        elif char == left:
            depth += 1
        elif char == right:
            depth -= 1
            if not depth:
                return text[start + 1:cursor], cursor + 1
        cursor += 1
    raise ValueError('Unbalanced source table/function')


def _split(body):
    pieces, start, depth, quote, cursor = [], 0, 0, None, 0
    while cursor < len(body):
        char = body[cursor]
        if quote:
            if char == '\\':
                cursor += 2
                continue
            if char == quote:
                quote = None
        elif char in '\"\'':
            quote = char
        elif char in '([{':
            depth += 1
        elif char in ')]}':
            depth -= 1
        elif char == ',' and depth == 0:
            pieces.append(body[start:cursor].strip())
            start = cursor + 1
        cursor += 1
    if quote or depth:
        raise ValueError('Unbalanced table row')
    pieces.append(body[start:].strip())
    return pieces


def _c_comments(text):
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*'
    return re.sub(tokens, lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group())
                  if m.group().startswith('/') else m.group(), text)


def _asm_comments(text):
    # Keep every source coordinate, including comments between a final native
    # tail jump and SECTION. A semicolon inside a quoted db string is data.
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|;[^\r\n]*'
    return re.sub(tokens, lambda m: ' ' * len(m.group())
                  if m.group().startswith(';') else m.group(), text)


def _asm_strings(text):
    return {match.group(1): match.group(2) or '' for match in re.finditer(
        r'^[\t ]*(_cstring_\w+):[\t ]*db[\t ]+(?:"([^"\r\n]*)"[\t ]*,[\t ]*)?0[\t ]*\r?$', text, re.M)}


def _asm_table(text, label, expected_line):
    match = _only(r'^' + re.escape(label) + r':\s*dd\s+([^\r\n]+)\r?$', text)
    if _line(text, match.start()) != expected_line:
        raise ValueError('Table coordinate changed: ' + label)
    return _split(match.group(1))


def _asm_function(text, symbol):
    begin = _only(r'^' + re.escape(symbol) + r':[\t ]*\r?$', text)
    for label in re.finditer(r'^(\w+):', text[begin.end():], re.M):
        if not label.group(1).startswith(symbol + '_'):
            body = text[begin.end():begin.end() + label.start()]
            return begin.end(), body.split('SECTION', 1)[0]
    raise ValueError('Cannot delimit native registry: ' + symbol)


def _asm_loop(body, symbol):
    begin = _only(r'^' + re.escape(symbol) + r':[\t ]*\r?$', body)
    rest = body[begin.end():]
    end = _only(r'^[\t ]*jnz[\t ]+' + re.escape(symbol) + r'[\t ]*\r?$', rest)
    return begin.end(), rest[:end.end()]


def _normalized(text):
    return re.sub(r'\s+', '', text)


def _require(body, fragments):
    compact = _normalized(body)
    for fragment in fragments:
        if _normalized(fragment) not in compact:
            raise ValueError('Registry layout/loop guard changed: ' + fragment)


def _validate_source(review, raw):
    if not isinstance(raw, bytes) or hashlib.sha256(raw).hexdigest() != review.sha256:
        raise ValueError('Reviewed pinned file hash changed: ' + review.path)
    text = raw.decode('utf-8', errors='strict')
    if review.family != 'client':
        text = _asm_comments(text)
    if review.family in ('hud', 'entity'):
        label, width = ('fields', 7) if review.family == 'hud' else ('ent_fields', 4)
        strings, words = _asm_strings(text), _asm_table(text, label, review.table_line)
        # The real HUD/entity objects have a null row plus four zero padding
        # words. Preserve that exact tail, instead of inventing a named row or
        # requiring all object padding to divide by the registered row stride.
        if len(words) != len(review.names) * width + review.null_tail_words:
            raise ValueError('Native row count/stride/null tail changed: ' + label)
        rows = []
        for offset in range(0, len(review.names) * width, width):
            row = words[offset:offset + width]
            if row[0] not in strings or not strings[row[0]]:
                raise ValueError('Unbound native field string')
            rows.append((strings[row[0]], tuple(row[1:])))
        if any(word not in ('0', '0x0') for word in words[len(review.names) * width:]):
            raise ValueError('Native table sentinel/padding is not null')
        symbol = 'GScr_AddFieldsForHudElems' if review.family == 'hud' else 'GScr_AddFieldsForEntity'
        start, body = _asm_function(text, symbol)
        call = _only(r'^[\t ]*call[\t ]+Scr_AddClassField[\t ]*\r?$', body)
        if _line(text, start + call.start()) != review.call_lines[0]:
            raise ValueError('Registration coordinate changed')
        if review.family == 'hud':
            _require(body, ('mov ebx, [fields]', 'mov esi, fields+0x1c',
                'mov [esp+0x8], eax', 'mov [esp+0x4], ebx', 'mov dword [esp], 0x1',
                'movzx eax, ax', 'add edi, 0x1c', 'add esi, 0x1c',
                'test ebx, ebx', 'jnz GScr_AddFieldsForHudElems_20'))
        else:
            _require(body, ('mov edx, [ent_fields]', 'mov ebx, ent_fields+0x10',
                'sar eax, 0x4', 'movzx eax, ax', 'mov [esp+0x8], eax',
                'mov [esp+0x4], edx', 'mov dword [esp], 0x0',
                'add esi, 0x10', 'add ebx, 0x10', 'test edx, edx',
                'jnz GScr_AddFieldsForEntity_20'))
            if not re.search(r'jmp\s+GScr_AddFieldsForClient\s*$', body):
                raise ValueError('Entity-to-client tail-call edge changed')
    elif review.family == 'client':
        clean = _c_comments(text)
        table = _only(r'^static\s+client_fields_s\s+fields\s*\[\s*\]\s*=\s*\{', clean)
        if _line(text, table.start()) != review.table_line:
            raise ValueError('Client table coordinate changed')
        body, _ = _balanced(clean, table.end() - 1)
        rows, cursor, sentinel = [], 0, False
        while cursor < len(body):
            if body[cursor].isspace() or body[cursor] == ',':
                cursor += 1
                continue
            if body[cursor] != '{' or sentinel:
                raise ValueError('Unexpected client table row or data after sentinel')
            row, cursor = _balanced(body, cursor)
            values = _split(row)
            if len(values) != 5:
                raise ValueError('Client row layout changed')
            if values[0] == 'NULL':
                if values != ['NULL', '0', 'F_INT', 'NULL', 'NULL']:
                    raise ValueError('Client sentinel changed')
                sentinel = True
                continue
            name = re.fullmatch(r'"([a-z]+)"', values[0])
            if not name:
                raise ValueError('Client field name is not a canonical literal')
            rows.append((name.group(1), tuple(values[1:])))
        if not sentinel:
            raise ValueError('Client sentinel missing')
        mask = _only(r'^#define\s+ENTFIELD_MASK\s+(0x[0-9a-fA-F]+)\s*$', clean)
        if int(mask.group(1), 16) != 0xc000:
            raise ValueError('Client field mask changed')
        function = _only(r'void\s+__cdecl\s+GScr_AddFieldsForClient\s*\(\s*\)\s*\{', clean)
        body, _ = _balanced(clean, function.end() - 1)
        call = _only(r'Scr_AddClassField\s*\(([^;]+)\);', body)
        if (_line(clean, function.end() + call.start()) != review.call_lines[0] or
            _normalized(call.group(1)) != '0,f->name,(uint16_t)(f-fields)|ENTFIELD_MASK'):
            raise ValueError('Client registration class/offset/coordinate changed')
        _require(body, ('for ( f = fields; f->name; ++f )',
                        'assert(!((f - fields) & ENTFIELD_MASK))',
                        'assert((f - fields) == (uint16_t)( f - fields ))'))
    elif review.family == 'filters':
        strings = _asm_strings(text)
        words = _asm_table(text, 'defaultGameWindowFilters', review.table_line)
        if (len(words) != 8 or any(word not in ('0', '0x0') for word in words[4:]) or
            tuple(strings.get(word) for word in words[:4]) != DEFAULTS):
            raise ValueError('Console default table changed')
        format_match = _only(r'^_cstring_con_gamemsgwindo:[\t ]*db[\t ]+"([^"]*)",0[\t ]*\r?$', text)
        if (_line(text, format_match.start()) != review.format_line or
            format_match.group(1) != 'con_gameMsgWindow%dFilter'):
            raise ValueError('Console format coordinate/value changed')
        for index, (symbol, loop) in enumerate((('Con_InitChannels', 'Con_InitChannels_50'),
                                               ('Con_InitGameMsgChannels', 'Con_InitGameMsgChannels_10'))):
            start, body = _asm_function(text, symbol)
            loop_start, loop_body = _asm_loop(body, loop)
            call = _only(r'^[\t ]*call[\t ]+Cvar_RegisterString[\t ]*\r?$', loop_body)
            if _line(text, start + loop_start + call.start()) != review.call_lines[index]:
                raise ValueError('Console registration coordinate changed')
            _require(body, ('xor ebx, ebx', 'defaultGameWindowFilters',
                'mov dword [esp+0x4], _cstring_con_gamemsgwindo', 'mov [esp+0x8], ebx',
                'call sprintf', 'mov [esp+0xc], edi', 'mov dword [esp+0x8], 0x29',
                'mov eax, [edx]', 'mov [esp+0x4], eax', 'mov [esp], esi',
                'add ebx, 0x1', 'cmp ebx, 0x4', 'jnz ' + loop))
        rows = [(format_match.group(1) % index, (DEFAULTS[index], '0x29')) for index in range(4)]
    else:
        raise ValueError('Unknown finite family')
    if tuple(name for name, metadata in rows) != review.names or len(set(review.names)) != len(review.names):
        raise ValueError('Finite name list/order/count changed')
    return rows


def _resolve(capabilities, reader, revision, reviews=REVIEWS):
    original = deepcopy(capabilities)
    errors, validated = [], []
    if revision != REVISION:
        return original, ['Finite enrollment server revision differs from reviewed pin']
    try:
        all_consumed = tuple(identifier for review in reviews for identifier in review.consumed())
        if len(all_consumed) != 7 or len(set(all_consumed)) != 7:
            raise ValueError('Finite consumption set is not the reviewed seven distinct entries')
        for review in reviews:
            rows = _validate_source(review, reader(review.path))
            if review.path.startswith(UPSTREAM_PATH + '/') is not True:
                raise ValueError('Resolution source path is not canonical pinned server source')
            for identifier in review.consumed():
                entry = original.get(identifier)
                if (not entry or entry.get('id') != identifier or entry.get('domain') != 'server' or
                    entry.get('kind') != identifier.split(':', 2)[1] or entry.get('resolved') is not False or
                    entry.get('status', 'pending') != 'pending' or entry.get('applicable', True) is not True):
                    raise ValueError('Expected opaque source entry missing/altered: ' + identifier)
            for line in review.call_lines:
                opaque = original[review.consumed()[review.call_lines.index(line)]]
                if not any(source.get('path') == review.path and source.get('line') == line and
                           source.get('sha256') == review.sha256 for source in opaque.get('provenance', [])):
                    raise ValueError('Opaque registration binding differs: ' + review.path)
            for name in review.names:
                identifier = review.id_prefix + name
                existing = original.get(identifier)
                if review.family == 'filters':
                    if existing is not None:
                        raise ValueError('New console-filter ID already exists; review required')
                elif (not existing or existing.get('id') != identifier or existing.get('domain') != 'server' or
                      existing.get('kind') != 'gsc_field' or existing.get('resolved') is not True or
                      not any(source.get('path') == review.path and source.get('sha256') == review.sha256
                              for source in existing.get('provenance', []))):
                    raise ValueError('Existing finite field binding differs: ' + identifier)
            validated.append((review, rows))
    except (OSError, ValueError, KeyError, TypeError, UnicodeError) as error:
        errors.append('Finite registration enrollment refused: ' + str(error))
        return original, errors
    result = deepcopy(original)
    entity_review = next(review for review, rows in validated if review.family == 'entity')
    entity_edges = deepcopy(original[entity_review.registry_id]['provenance'])
    for review, rows in validated:
        registration = [source for identifier in review.consumed()
                        for source in deepcopy(original[identifier]['provenance'])]
        for index, (name, row) in enumerate(rows):
            identifier = review.id_prefix + name
            if review.family == 'filters':
                result[identifier] = {'id': identifier, 'domain': 'server', 'kind': 'dvar',
                    'name': name, 'resolved': True, 'provenance': [], 'status': 'pending',
                    'applicable': True, 'reasons': ['Named source identity only; native filtering/archive/latch behavior remains unproven.']}
            entry = result[identifier]
            entry['provenance'].extend(deepcopy(registration))
            entry['inventory_resolution'] = {'kind': 'reviewed_finite_source_registration',
                'semantic_proof': False, 'revision': REVISION, 'family': review.family,
                'source_sha256': review.sha256, 'source_path': review.path,
                'row_index': index, 'row_definition': list(row), 'registration_ids': list(review.consumed())}
            if review.family != 'filters':
                entry['inventory_resolution']['class'] = 1 if review.family == 'hud' else 0
                entry['inventory_resolution']['field_offset'] = index | (0xc000 if review.family == 'client' else 0)
            if review.family == 'client':
                entry['provenance'].extend(deepcopy(entity_edges))
                entry['inventory_resolution']['registration_edge'] = 'GScr_AddFieldsForEntity -> GScr_AddFieldsForClient'
            elif review.family == 'entity':
                entry['inventory_resolution']['tail_call'] = 'GScr_AddFieldsForClient'
        for identifier in review.consumed():
            del result[identifier]
    return result, errors


def enroll_finite_registrations(capabilities, root, upstream):
    """Guarded production insertion seam; no config/evidence/status overrides."""
    if upstream.get('path') != UPSTREAM_PATH:
        return deepcopy(capabilities), ['Finite enrollment checkout path differs from reviewed path']
    root = Path(root).resolve()

    def reader(relative):
        path = (root / relative).resolve()
        if not path.is_relative_to(root) or path.relative_to(root).as_posix() != relative:
            raise ValueError('Pinned source path escaped or used an alias')
        return path.read_bytes()

    return _resolve(capabilities, reader, upstream.get('revision'))


INTEGRATION = '''
# After raw capabilities/source provenance aggregation, before config checks:
capabilities, finite_errors = enroll_finite_registrations(
    capabilities, root, config['upstreams'].get('server', {}))
errors.extend(finite_errors)
# Existing verify_evidence remains the sole source of semantic status.
'''
