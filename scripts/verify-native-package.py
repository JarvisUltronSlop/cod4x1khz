"""Verify native packaged PE files, local import ABIs, and preserved Win32 hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', default='results/x64-packaged-binaries.json')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
native_paths = [root / 'runtime/x64' / name for name in (
    'cod4x1khz.exe', 'cod4x1khz_bench.exe', 'cod4x1khz_server.exe', 'cod4x1khz_update.exe',
    'avcodec-62.dll', 'avformat-62.dll', 'avutil-60.dll', 'D3DX9_43.dll', 'swresample-6.dll', 'swscale-9.dll')]
native_paths.append(root / 'runtime/plugins/x64/khz_regression.dll')
preserved = {
    'runtime/cod4x1khz.exe': 'fa9baca2076ec15275ef2474a9587497a32aa3ec5c0310def49ec056459db577',
    'runtime/cod4x1khz_server.exe': '08090f4625e91ae4490fd93d3cf1cdb04ab29d4dfcad193b21a2cf8bdf7b15cd',
}
def inspect(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'Invalid PE: {path}')
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    if machine != 0x8664 or magic != 0x20b:
        raise ValueError(f'Expected native AMD64/PE32+: {path}')
    section_table = optional + size
    def offset(rva):
        for index in range(sections):
            at = section_table + index * 40
            virtual_size, address, raw_size, raw = struct.unpack_from('<IIII', data, at + 8)
            if address <= rva < address + max(virtual_size, raw_size):
                return raw + rva - address
        raise ValueError(f'Unmapped RVA {rva:x}: {path}')
    import_rva = struct.unpack_from('<I', data, optional + 112 + 8)[0]
    imports = []
    if import_rva:
        cursor = offset(import_rva)
        for _ in range(256):
            entry = struct.unpack_from('<IIIII', data, cursor)
            if not any(entry):
                break
            name = offset(entry[3])
            imports.append(data[name:data.index(b'\0', name)].decode('ascii'))
            cursor += 20
        else:
            raise ValueError('Import table exceeds bounded parser')
    return {'machine': 'AMD64', 'format': 'PE32+', 'sha256': hashlib.sha256(data).hexdigest(), 'imports': imports}
report = {'native': {}, 'preserved_win32': {}, 'local_import_abis_verified': True}
for path in native_paths:
    result = inspect(path)
    report['native'][str(path.relative_to(root))] = result
    candidates = {p.name.lower(): p for p in (root / 'runtime/x64').iterdir() if p.is_file()}
    candidates.update({p.name.lower(): p for p in path.parent.iterdir() if p.is_file()})
    for dependency in result['imports']:
        if dependency.lower() in candidates:
            inspect(candidates[dependency.lower()])
for path, expected in preserved.items():
    actual = hashlib.sha256((root / path).read_bytes()).hexdigest()
    if actual != expected:
        raise ValueError(f'Accepted Win32 binary changed: {path}')
    report['preserved_win32'][path] = actual
destination = root / args.output
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text(json.dumps(report, indent=2) + '\n')
print(f'Verified {len(native_paths)} native PE files, local import ABIs and {len(preserved)} preserved Win32 binaries')
