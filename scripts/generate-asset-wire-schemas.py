"""Generate explicit retail-record converters from paired compiler catalogs.

The x86 catalog supplies fixed disk offsets. Native offsets remain C++ offsetof
expressions, with assertions against the measured ABI. Unions with incompatible
member layouts require explicit discriminators; unknown layouts fail generation.
"""
import argparse
from pathlib import Path
import json
import re
import importlib.util
inventory_spec = importlib.util.spec_from_file_location('asset_inventory', Path(__file__).with_name('asset-loader-inventory.py'))
inventory_module = importlib.util.module_from_spec(inventory_spec)
inventory_spec.loader.exec_module(inventory_module)
Catalog = inventory_module.Catalog


class Schemas:
    def __init__(self, disk, native):
        self.disk, self.native = disk, native

    def same(self, a, b, seen=None):
        a, b = self.disk.unalias(a), self.native.unalias(b)
        if a['tag'] != b['tag'] or a['bytes'] != b['bytes']: return False
        if a['tag'] == 14: return False
        if a['tag'] in (12, 16): return True
        if a['tag'] == 15:
            return a['count'] == b['count'] and self.same(a['target'], b['target'])
        if a['tag'] != 11: raise ValueError(f'Unsupported type {a}')
        if len(a['fields']) != len(b['fields']): return False
        return all(x['name'] == y['name'] and x['offset'] == y['offset'] and
                   x['bit_position'] == y['bit_position'] and x['bit_length'] == y['bit_length'] and
                   self.same(x['type'], y['type']) for x, y in zip(a['fields'], b['fields']))

    def flatten(self, ai, bi, root, path='', wo=0, no=0, limit=None, conditions=()):
        a, b = self.disk.unalias(ai), self.native.unalias(bi)
        if limit is not None and wo >= limit: return []
        if a['tag'] != b['tag']: raise ValueError(f'Type mismatch: {root}.{path}')
        extent = a['bytes']
        if limit is not None: extent = min(extent, limit-wo)
        def leaf(size, kind='Bytes'):
            if size != a['bytes'] and kind == 'Pointer32': raise ValueError('Partial disk pointer')
            return [{'wire': wo, 'native': no, 'bytes': size, 'kind': kind,
                     'path': path, 'conditions': conditions}]
        if a['tag'] == 14:
            if a['bytes'] != 4 or b['bytes'] != 8: raise ValueError('Expected x86/x64 pointer pair')
            return leaf(4, 'Pointer32')
        # This retail runtime-block field was represented as an int in the
        # original headers even though it stores an ODE body address.
        if root == 'DynEntityClient' and path == 'physObjId' and a['bytes'] == 4 and b['bytes'] == 8:
            return leaf(4, 'Pointer32')
        # Child addresses in this contiguous tree array are relative record
        # offsets; widening its pointer member changes the array stride.
        if root == 'GfxAabbTree' and path == 'childrenOffset':
            return leaf(4, 'RelativeRecord32')
        if self.same(ai, bi): return leaf(extent)
        if a['tag'] in (12, 16): raise ValueError(f'Scalar ABI changed: {root}.{path}')
        if a['tag'] == 15:
            if a['count'] != b['count']: raise ValueError('Array length changed')
            aw, bn = self.disk.unalias(a['target']), self.native.unalias(b['target'])
            result=[]
            for i in range(a['count']):
                result += self.flatten(a['target'], b['target'], root, f'{path}[{i}]',
                    wo+i*aw['bytes'], no+i*bn['bytes'], limit, conditions)
            return result
        if a['tag'] != 11: raise ValueError(f'Unsupported record: {root}.{path}')
        fields_a, fields_b = a['fields'], b['fields']
        if [f['name'] for f in fields_a] != [f['name'] for f in fields_b]:
            raise ValueError(f'Members changed: {root}.{path}')
        pairs = list(zip(fields_a, fields_b))
        if a['kind'] == 2:
            # Four-byte unions hold scalar bits or one encoded pointer. Widening
            # that slot preserves every scalar's original lower 32 bits.
            if a['bytes'] == 4 and b['bytes'] == 8:
                for x,y in pairs:
                    if self.disk.unalias(x['type'])['tag'] == 14:
                        return self.flatten(x['type'],y['type'],root,
                            '.'.join(filter(None,[path,x['name']])),wo,no,limit,conditions)
            selector = self.selector(a['name'], root, path, wo)
            if selector:
                result=[]
                for x,y in pairs:
                    condition=selector.get(x['name'])
                    if condition is None: raise ValueError(f'Unselected union member {a["name"]}.{x["name"]}')
                    result += self.flatten(x['type'],y['type'],root,
                        '.'.join(filter(None,[path,x['name']])),wo,no,limit,conditions+(condition,))
                return result
            alternatives=[self.flatten(x['type'],y['type'],root,
                '.'.join(filter(None,[path,x['name']])),wo,no,limit,conditions) for x,y in pairs]
            signatures=[[(f['wire'],f['native'],f['bytes'],f['kind'],f['conditions']) for f in fs] for fs in alternatives]
            if signatures and all(s == signatures[0] for s in signatures): return alternatives[0]
            raise ValueError(f'Union requires discriminator: {root}.{path} ({a["name"]})')
        result=[]
        for x,y in pairs:
            if x['bitfield'] or y['bitfield']: raise ValueError(f'Changed bitfield record: {root}.{path}')
            result += self.flatten(x['type'],y['type'],root,
                '.'.join(filter(None,[path,x['name']])),wo+x['offset'],no+y['offset'],limit,conditions)
        return result

    def selector(self, name, root, path, union_offset):
        if name == 'SoundFileRef' and root == 'SoundFile':
            return {'loadSnd':'readScalar(wire, 0, 1) == 1', 'streamSnd':'readScalar(wire, 0, 1) != 1'}
        if name == 'pathnode_tree_info_t' and root == 'pathnode_tree_t':
            return {'s':'readScalar(wire, 0, 4) & 0x80000000u',
                    'child':'!(readScalar(wire, 0, 4) & 0x80000000u)'}
        if name == 'cLeafBrushNodeData_t' and root == 'cLeafBrushNode_s':
            return {'leaf':'static_cast<std::int16_t>(readScalar(wire, 2, 2)) > 0',
                    'children':'static_cast<std::int16_t>(readScalar(wire, 2, 2)) <= 0'}
        if name == 'entryInternalData' and root == 'expressionEntry':
            return {'op':'readScalar(wire, 0, 4) == 0', 'operand':'readScalar(wire, 0, 4) != 0'}
        return None


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disk',type=Path); parser.add_argument('native',type=Path)
    parser.add_argument('inventory',type=Path); parser.add_argument('output',type=Path)
    args=parser.parse_args()
    disk,native=Catalog(args.disk),Catalog(args.native)
    generator=Schemas(disk,native)
    report=json.loads(args.inventory.read_text())
    records=report['record_types']
    # Variable-length records read only the prefix preceding their inline tail.
    prefixes={'XAnimDeltaPartQuat':(4,'u'), 'XAnimDeltaPartQuatDataFrames':(4,'indices'),
              'XAnimPartTrans':(4,'u'), 'XAnimPartTransFrames':(28,'indices'),
              'MaterialTechnique':(8,'passArray'), 'GfxImageLoadDef':(16,'data')}
    lines=['// GENERATED by scripts/generate-asset-wire-schemas.py. Do not edit.',
           '// Retail wire sizes are fixed. Native offsets use the compiling ABI.',
           '#pragma once', '#include <asset_wire.hpp>', '#include <asset_schema.hpp>',
           'namespace khz::asset_wire {']
    failures=[]; total=0
    for name,pair in sorted(records.items()):
        a,b=pair['wire'],pair['native']
        size,end=prefixes.get(name,(a['bytes'],None))
        try:
            fields=generator.flatten(a['id'],b['id'],name,limit=size)
            if not fields: raise ValueError('No fields')
            native_size=f'offsetof({name}, {end})' if end else f'sizeof({name})'
            expected_native=next(f['offset'] for f in b['fields'] if f['name']==end) if end else b['bytes']
            lines += [f'inline Layout schema_{name}(std::span<const std::byte> wire, std::vector<Field>& fields) {{',
                      f'    static_assert(sizeof({name}) == (sizeof(void*) == 8 ? {b["bytes"]} : {a["bytes"]}));',
                      '    fields.clear();']
            unique={}
            for field in fields:
                path=field['path']
                offset=f'offsetof({name}, {path})' if path else '0'
                if path not in unique:
                    unique[path]=True
                    lines.append(f'    static_assert({offset} == (sizeof(void*) == 8 ? {field["native"]} : {field["wire"]}));')
                condition=' && '.join(f'({c})' for c in field['conditions'])
                statement=f'fields.push_back({{{field["wire"]}, {offset}, {field["bytes"]}, FieldKind::{field["kind"]}}});'
                lines.append('    '+(f'if ({condition}) ' if condition else '')+statement)
            lines += [f'    static_assert({native_size} == (sizeof(void*) == 8 ? {expected_native} : {size}));',
                      f'    return {{{size}, {native_size}, fields}};', '}',
                      f'inline constexpr Schema record_{name}{{"{name}", {size}, {native_size}, schema_{name}, {str(any(f["conditions"] for f in fields)).lower()}}};']
            total+=len(fields)
        except (ValueError,KeyError) as error: failures.append(f'{name}: {error}')
    if failures:
        print('\n'.join(failures)); raise SystemExit(1)
    lines+=['}']
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('\n'.join(lines)+'\n')
    print(f'Generated {len(records)} schemas and {total} mapped fields')

if __name__ == '__main__': main()
