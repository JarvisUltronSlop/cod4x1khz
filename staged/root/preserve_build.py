"""Retain exact immutable build binaries/package before the next deployment."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=int)
    args = parser.parse_args()
    archive = ROOT / f'results/build{args.build}-source-and-binary-hashes.json'
    record = json.loads(archive.read_text(encoding='utf-8-sig'))
    if record.get('build') != args.build or record.get('tests', {}).get('passed') is not True:
        raise ValueError('Successful exact build record required')
    files = {**record['binary_sha256'], **record['runtime_asset_packages']}
    destination = ROOT / f'results/build{args.build}-release-binaries'
    if destination.exists():
        raise ValueError('Preserved build already exists; never overwrite it')
    for name, expected in files.items():
        source = ROOT / name
        target = destination / name
        if not source.resolve().is_relative_to(ROOT) or not target.resolve().is_relative_to(destination):
            raise ValueError('Path escaped preservation root')
        if digest(source) != expected:
            raise ValueError('Current binary/package differs from archive: ' + name)
    destination.mkdir()
    for name, expected in files.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, target)
        if digest(target) != expected:
            raise ValueError('Preserved bytes differ: ' + name)
    shutil.copyfile(archive, destination / 'source-and-binary-hashes.json')
    (destination / 'preservation.json').write_text(json.dumps({'build': args.build,
        'original_archive_sha256': digest(archive), 'preserved_files': len(files), 'verified': True,
        'created_utc': datetime.now(timezone.utc).isoformat()}, indent=2) + '\n')
    print(json.dumps({'preserved': True, 'build': args.build, 'files': len(files)}))

if __name__ == '__main__':
    main()
