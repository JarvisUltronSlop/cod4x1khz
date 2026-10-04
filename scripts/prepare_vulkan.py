"""Fetch checksum-pinned Vulkan build sources inside this project only."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import urllib.request


def prepare(root, validation=False):
    lock = json.loads((root / 'upstream-lock.json').read_text())
    entries = dict(lock['Vulkan'])
    if validation:
        entries.update(lock['VulkanValidation'])
    home = root / '.deps' / 'vulkan'
    home.mkdir(parents=True, exist_ok=True)
    receipt = {}
    for name, spec in entries.items():
        archive = home / (name + '.tar.gz')
        destination = home / name
        marker = destination / '.cod4x1khz-source.json'
        installed = marker.is_file()
        if installed:
            existing = json.loads(marker.read_text())
            if existing != spec:
                raise RuntimeError(f'Existing {name} source differs from its pin; preserve it for inspection')
        if not archive.is_file():
            request = urllib.request.Request(spec['archive'], headers={'User-Agent': 'cod4x1khz-build'})
            partial = archive.with_suffix('.partial')
            with urllib.request.urlopen(request, timeout=60) as source, partial.open('wb') as output:
                shutil.copyfileobj(source, output)
            partial.replace(archive)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != spec['sha256']:
            raise RuntimeError(f'{name} archive checksum mismatch')
        if installed:
            receipt[name] = existing
            continue
        if destination.exists():
            raise RuntimeError(f'Incomplete {name} destination exists; preserve it for inspection')
        with tarfile.open(archive, 'r:gz') as source:
            members = source.getmembers()
            tops = {PurePosixPath(m.name).parts[0] for m in members}
            if len(tops) != 1:
                raise RuntimeError(f'{name} archive has multiple roots')
            for member in members:
                parts = PurePosixPath(member.name).parts
                if member.name.startswith('/') or '..' in parts or '\\' in member.name or ':' in member.name:
                    raise RuntimeError(f'{name} archive contains an unsafe path')
                if not (member.isfile() or member.isdir()):
                    raise RuntimeError(f'{name} archive contains an unsupported link or file type')
            destination.mkdir()
            for member in members:
                target = destination.joinpath(*PurePosixPath(member.name).parts[1:])
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as content, target.open('wb') as output:
                        shutil.copyfileobj(content, output)
        verified = dict(spec, sha256=digest)
        marker.write_text(json.dumps(verified, indent=2) + '\n')
        receipt[name] = verified
        print(f'{name}: verified {digest}', flush=True)
    (home / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validation', action='store_true', help='Also fetch optional pinned validation-layer sources')
    args = parser.parse_args()
    prepare(Path(__file__).resolve().parent.parent, args.validation)


if __name__ == '__main__':
    main()
