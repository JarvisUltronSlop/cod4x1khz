"""Integrate per-frame CPU geometry staging at the existing renderer transfer."""
from pathlib import Path
import hashlib
import json
root = Path(__file__).resolve().parents[2]
record = json.loads((root / 'results/build79-source-and-binary-hashes.json').read_text(encoding='utf-8-sig'))
changes = {}
def edit(name, before, after, include=False):
    raw = (root / name).read_bytes()
    if record['source_sha256'][name] != hashlib.sha256(raw).hexdigest():
        raise ValueError('Source differs from archived Build 79: ' + name)
    text = changes.get(name, raw.decode('utf-8').replace('\r\n', '\n'))
    if include and '#include <khz/khz_frontend_upload.h>' not in text:
        text = '#ifdef COD4X1KHZ\n#include <khz/khz_frontend_upload.h>\n#endif\n' + text
    if text.count(before) != 1:
        raise ValueError('Missing or ambiguous hook in ' + name + ': ' + before[:90])
    changes[name] = text.replace(before, after, 1)

edit('CMakeLists.txt', '  target_sources(KisakCOD-mp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_render_profile.cpp")',
     '  target_sources(KisakCOD-mp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_render_profile.cpp"\n    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_frontend_upload.cpp")')
buffers = 'engine/src/gfx_d3d/r_buffers.cpp'
edit(buffers, '    gfxBuf.preTessBufferFrame = 0;\n}', '    gfxBuf.preTessBufferFrame = 0;\n#ifdef COD4X1KHZ\n    KHZ_FrontendUploadInit();\n#endif\n}', include=True)
edit(buffers, 'void __cdecl R_DestroyDynamicBuffers()\n{', 'void __cdecl R_DestroyDynamicBuffers()\n{\n#ifdef COD4X1KHZ\n    KHZ_FrontendUploadShutdown();\n#endif')
for kind in ['Vertex', 'Index']:
    anchor = '    hr = handle->Lock(offset, bytes, &bufferData, lockFlags);'
    # Select the particular native function, not the identical other Lock call.
    text = changes[buffers]
    signature = 'void *__cdecl R_Lock' + kind + 'Buffer('
    start = text.index(signature)
    end = text.index('\n}', start)
    body = text[start:end]
    if body.count(anchor) != 1: raise ValueError('Native lock anchor differs')
    body = body.replace(anchor, '#ifdef COD4X1KHZ\n    if (KHZ_Frontend' + kind + 'Lock(handle, offset, bytes, lockFlags, &bufferData)) return bufferData;\n#endif\n' + anchor)
    changes[buffers] = text[:start] + body + text[end:]
    signature = 'void __cdecl R_Unlock' + kind + 'Buffer(IDirect3D' + kind + 'Buffer9' + ('* handle)' if kind == 'Vertex' else ' *handle)') + '\n{'
    edit(buffers, signature, signature + '\n#ifdef COD4X1KHZ\n    if (KHZ_Frontend' + kind + 'Unlock(handle)) return;\n#endif')
edit('engine/src/gfx_d3d/rb_backend.cpp', 'void __cdecl RB_BeginFrame(const GfxBackEndData *data)\n{',
     'void __cdecl RB_BeginFrame(const GfxBackEndData *data)\n{\n#ifdef COD4X1KHZ\n    KHZ_FrontendUpload(data);\n#endif', include=True)
edit('engine/src/khz/khz_render_profile.cpp', '#include "khz_render_profile.h"', '#include "khz_render_profile.h"\n#include "khz_frontend_upload.h"')
edit('engine/src/khz/khz_render_profile.cpp', '    if (!std::strcmp(action, "status")) return status();',
     '    if (!std::strcmp(action, "uploads")) return KHZ_FrontendUploadStatus();\n'
     '    if (!std::strcmp(action, "uploadmode 0") || !std::strcmp(action, "uploadmode 1")) {\n'
     '        Dvar_SetBoolByName("r_frontendBufferStaging", action[11] == \'1\');\n'
     '        return KHZ_FrontendUploadStatus();\n    }\n'
     '    if (!std::strcmp(action, "status")) return status();')
backup = root / 'staged/render-profile/before80'
if backup.exists(): raise ValueError('Refuse to overwrite retained native source')
before = {}
for name, text in changes.items():
    path = root / name; raw = path.read_bytes()
    before[name] = hashlib.sha256(raw).hexdigest()
    save = backup / name; save.parent.mkdir(parents=True, exist_ok=True); save.write_bytes(raw)
    path.write_bytes(text.replace('\n', '\r\n' if b'\r\n' in raw else '\n').encode('utf-8'))
(root / 'staged/render-profile/application80.json').write_text(json.dumps({
    'applied': True, 'gameplay_changed': False, 'before_sha256': before,
    'after_sha256': {n: hashlib.sha256((root / n).read_bytes()).hexdigest() for n in changes}}, indent=2) + '\n')
print(json.dumps({'applied': True, 'files': len(changes)}))
