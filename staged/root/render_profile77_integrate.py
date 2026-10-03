"""Extend the disabled-by-default native capture with actual frame sub-stages."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[2]
record = json.loads((root / 'results/build76-source-and-binary-hashes.json').read_text(encoding='utf-8-sig'))
changes = {}

def hook(name, signature, stage):
    raw = (root / name).read_bytes()
    if record['source_sha256'][name] != hashlib.sha256(raw).hexdigest():
        raise ValueError('Native source differs from Build 76: ' + name)
    text = changes.get(name, raw.decode('utf-8').replace('\r\n', '\n'))
    if '#include <khz/khz_render_profile.h>' not in text:
        text = '#ifdef COD4X1KHZ\n#include <khz/khz_render_profile.h>\n#endif\n' + text
    if text.count(signature) != 1:
        raise ValueError('Missing or ambiguous function: ' + signature)
    text = text.replace(signature, signature + '\n#ifdef COD4X1KHZ\n    KHZ_RenderProfileScope stageTrace(KHZ_RenderStage::' + stage + ');\n#endif', 1)
    changes[name] = text

hook('engine/src/gfx_d3d/r_init.cpp', 'void __cdecl R_SyncGpu(int(__cdecl *WorkCallback)(unsigned __int64))\n{', 'GpuSync')
hook('engine/src/gfx_d3d/r_rendercmds.cpp', 'void __cdecl R_BeginFrame()\n{', 'BeginFrame')
hook('engine/src/cgame_mp/cg_view_mp.cpp', '    int32_t renderScreen)\n{', 'Cgame')
hook('engine/src/gfx_d3d/r_scene.cpp', 'void __cdecl R_RenderScene(const refdef_s *refdef)\n{', 'RenderScene')
hook('engine/src/cgame_mp/cg_ents_mp.cpp', 'int32_t __cdecl CG_AddPacketEntities(int32_t localClientNum)\n{', 'PacketEntities')
hook('engine/src/cgame_mp/cg_draw_mp.cpp', 'void __cdecl CG_Draw2D(int32_t localClientNum)\n{', 'Draw2D')
hook('engine/src/cgame_mp/cg_draw_mp.cpp', 'void __cdecl CG_DrawActive(int32_t localClientNum)\n{', 'DrawActive')
view = 'engine/src/cgame_mp/cg_view_mp.cpp'
call = '                CG_AddViewWeapon(localClientNum);'
if changes[view].count(call) != 1:
    raise ValueError('Missing or ambiguous view weapon call')
changes[view] = changes[view].replace(call, '                {\n#ifdef COD4X1KHZ\n                    KHZ_RenderProfileScope weaponTrace(KHZ_RenderStage::ViewWeapon);\n#endif\n                    CG_AddViewWeapon(localClientNum);\n                }', 1)
backup = root / 'staged/render-profile/before77'
if backup.exists():
    raise ValueError('Refuse to overwrite retained native source')
before = {}
for name, text in changes.items():
    path = root / name
    raw = path.read_bytes()
    before[name] = hashlib.sha256(raw).hexdigest()
    saved = backup / name
    saved.parent.mkdir(parents=True, exist_ok=True)
    saved.write_bytes(raw)
    path.write_bytes(text.replace('\n', '\r\n' if b'\r\n' in raw else '\n').encode('utf-8'))
(root / 'staged/render-profile/application77.json').write_text(json.dumps({
    'applied': True, 'diagnostic_default': 'off', 'gameplay_changed': False,
    'before_sha256': before, 'after_sha256': {n: hashlib.sha256((root / n).read_bytes()).hexdigest() for n in changes}
}, indent=2) + '\n')
print(json.dumps({'applied': True, 'files': len(changes)}))
