"""Compile the actual staging implementation with controlled native API boundaries.

The fake D3D buffers establish copies, bounds and owner transitions, not GPU
correctness, throughput, or the full engine ABI. Live probes are separate.
"""
from pathlib import Path
import hashlib
import json
root = Path(__file__).resolve().parents[3]
source = root / 'engine/src/khz/khz_frontend_upload.cpp'
text = source.read_text()
native = text[text.index('namespace {'):]
dest = Path(__file__).parent / 'generated'
dest.mkdir(exist_ok=True)
(dest / 'native_upload.inc').write_text(native)
(dest / 'extraction.json').write_text(json.dumps({'native_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'extracted_body_sha256': hashlib.sha256(native.encode()).hexdigest(),
    'scope': 'Actual function bodies with fake D3D, dvar, allocator, and thread-role boundaries. No native GPU/ABI/performance claim.'}, indent=2) + '\n')
print(json.dumps({'extracted': True, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}))
