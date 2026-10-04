"""Match production loader reads to compiler-measured disk/native record types."""
import argparse
import json
from pathlib import Path
import re

class Catalog:
    def __init__(self, path):
        data = json.loads(path.read_text())
        self.types = {t['id']: t for t in data['types']}
        self.globals = {g['name']: g['type'] for g in data['globals']}
    def unalias(self, index):
        seen = set()
        while self.types[index]['tag'] == 17:
            if index in seen: raise ValueError('Recursive typedef')
            seen.add(index); index = self.types[index]['target']
        return self.types[index]
    def pointee(self, name):
        pointer = self.unalias(self.globals[name])
        if pointer['tag'] != 14: raise ValueError(f'{name} is not a pointer')
        return self.unalias(pointer['target'])

def arguments(text):
    result = []; start = depth = 0
    for i, char in enumerate(text):
        if char in '([': depth += 1
        elif char in ')]': depth -= 1
        elif char == ',' and not depth:
            result.append(text[start:i].strip()); start = i + 1
    result.append(text[start:].strip())
    if len(result) not in (3, 4): raise ValueError(f'Cannot parse loader call: {text}')
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('win32', type=Path); parser.add_argument('x64', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (root/'engine/src/database/db_load.cpp').read_text()
    disk, native = Catalog(args.win32), Catalog(args.x64)
    reads = []; missing = []; types = {}
    for call in re.finditer(r'\b(Load_Stream|DB_LoadTypedStream|DB_LoadPointerStream)\((.*?)\);', source, re.S):
        start, pointer, size = arguments(call[2])[:3]
        variable = re.search(r'\bvar\w+\b', pointer)
        row = {'line': source.count('\n', 0, call.start()) + 1, 'loader':call[1], 'start': start,
               'pointer': pointer, 'wire_expression': size}
        try:
            if not variable: raise ValueError('No source-owned loader variable')
            name = variable[0]; a, b = disk.pointee(name), native.pointee(name)
            if a['tag'] != b['tag']: raise ValueError('Disk/native type kinds differ')
            row.update(variable=name, type=a['name'] or ('pointer' if a['tag']==14 else f'primitive-{a["base"]}'),
                       tag=a['tag'], wire_type_bytes=a['bytes'], native_type_bytes=b['bytes'])
            if a['tag'] == 11:
                if a['name'] != b['name']: raise ValueError('Disk/native record names differ')
                types[a['name']] = {'wire': a, 'native': b}
            row['native_size_expression'] = 'sizeof' in size
        except (KeyError, ValueError) as error:
            row['error'] = str(error); missing.append(row)
        reads.append(row)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'schema': 1, 'reads': reads, 'record_types': types,
              'unmatched_reads': missing, 'game_port_complete': False}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(reads)} production reads; {len(types)} compiler-measured record types; {len(missing)} unmatched reads')
    if missing: raise SystemExit(1)

if __name__ == '__main__': main()
