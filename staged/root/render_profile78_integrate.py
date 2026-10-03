"""Source-bound, diagnostic-only drill-down of the measured scene-preparation cost."""
from pathlib import Path
import hashlib
import json
root = Path(__file__).resolve().parents[2]
record = json.loads((root / 'results/build77-source-and-binary-hashes.json').read_text(encoding='utf-8-sig'))
changes = {}
def edit(name, before, after, include=True):
    path = root / name
    raw = path.read_bytes()
    if record['source_sha256'][name] != hashlib.sha256(raw).hexdigest():
        raise ValueError('Source differs from archived Build 77: ' + name)
    text = changes.get(name, raw.decode('utf-8').replace('\r\n', '\n'))
    if include and '#include <khz/khz_render_profile.h>' not in text:
        text = '#ifdef COD4X1KHZ\n#include <khz/khz_render_profile.h>\n#endif\n' + text
    if text.count(before) != 1:
        raise ValueError('Missing or ambiguous hook: ' + before)
    changes[name] = text.replace(before, after, 1)
def scope(stage):
    return '\n#ifdef COD4X1KHZ\n    KHZ_RenderProfileScope detailTrace(KHZ_RenderStage::' + stage + ');\n#endif'
def hook(name, signature, stage):
    edit(name, signature, signature + scope(stage))

stages = ['VertexLock', 'IndexLock', 'WorldDpvs', 'Bsp', 'StaticModels', 'DynModels', 'SceneEntities',
          'ModelLighting', 'MergeSurfs', 'Sound', 'FxPhysics', 'DynEntityPhysics', 'FrameWork', 'ClientFrame', 'EndFrame']
edit('engine/src/khz/khz_render_profile.h', 'Draw2D, DrawActive, Count', 'Draw2D, DrawActive,\n    ' + ', '.join(stages) + ', Count', include=False)
edit('engine/src/khz/khz_render_profile.cpp', 'CpuCapacity = 131072', 'CpuCapacity = 524288', include=False)
edit('engine/src/khz/khz_render_profile.cpp', '"draw_2d", "draw_active"};', '"draw_2d", "draw_active",\n    ' + ', '.join('"' + n + '"' for n in ['vertex_lock','index_lock','world_dpvs','bsp','static_models','dyn_models','scene_entities','model_lighting','merge_surfs','sound','fx_physics','dyn_entity_physics','frame_work','client_frame','end_frame']) + '};', include=False)
hook('engine/src/gfx_d3d/r_buffers.cpp', 'void *__cdecl R_LockVertexBuffer(IDirect3DVertexBuffer9 *handle, int offset, int bytes, int lockFlags)\n{', 'VertexLock')
hook('engine/src/gfx_d3d/r_buffers.cpp', 'void *__cdecl R_LockIndexBuffer(IDirect3DIndexBuffer9 *handle, int offset, int bytes, int lockFlags)\n{', 'IndexLock')
hook('engine/src/gfx_d3d/r_rendercmds.cpp', 'void __cdecl R_EndFrame()\n{', 'EndFrame')
hook('engine/src/gfx_d3d/r_model_lighting.cpp', 'void __cdecl R_SetAllStaticModelLighting()\n{', 'ModelLighting')
hook('engine/src/gfx_d3d/r_pretess.cpp', 'void __cdecl R_MergeAndEmitDrawSurfLists(DrawSurfType firstStage, int stageCount)\n{', 'MergeSurfs')
hook('engine/src/client_mp/cl_main_mp.cpp', 'void __cdecl CL_UpdateSound()\n{', 'Sound')
hook('engine/src/client_mp/cl_main_mp.cpp', 'void __cdecl CL_Frame(int localClientNum)\n{', 'ClientFrame')
edit('engine/src/qcommon/common.cpp', '    KHZ_TimingScope frameWorkTimer(KHZ_Timing::FrameWork);', '    KHZ_TimingScope frameWorkTimer(KHZ_Timing::FrameWork);' + scope('FrameWork'))
for label, stage in [('R_AddWorldSurfacesDpvs','WorldDpvs'), ('bsp surfaces','Bsp'), ('R_DrawAllDynEnt','DynModels')]:
    anchor = '        PROF_SCOPED("' + label + '");'
    edit('engine/src/gfx_d3d/r_scene.cpp', anchor, anchor + scope(stage))
for call, stage in [('R_AddAllStaticModelSurfacesCamera();','StaticModels'), ('R_DrawAllSceneEnt(viewInfo);','SceneEntities'),
                    ('FX_RunPhysics(viewInfo->localClientNum);','FxPhysics'), ('DynEntCl_ProcessEntities(viewInfo->localClientNum);','DynEntityPhysics')]:
    anchor = '    ' + call
    edit('engine/src/gfx_d3d/r_scene.cpp', anchor, '    {' + scope(stage) + '\n        ' + call + '\n    }')
backup = root / 'staged/render-profile/before78'
if backup.exists():
    raise ValueError('Refuse to replace retained source')
before = {}
for name, text in changes.items():
    path = root / name; raw = path.read_bytes()
    before[name] = hashlib.sha256(raw).hexdigest()
    save = backup / name; save.parent.mkdir(parents=True, exist_ok=True); save.write_bytes(raw)
    path.write_bytes(text.replace('\n', '\r\n' if b'\r\n' in raw else '\n').encode('utf-8'))
(root / 'staged/render-profile/application78.json').write_text(json.dumps({'applied': True,
    'diagnostic_default': 'off', 'gameplay_changed': False, 'before_sha256': before,
    'after_sha256': {n: hashlib.sha256((root / n).read_bytes()).hexdigest() for n in changes}}, indent=2) + '\n')
print(json.dumps({'applied': True, 'files': len(changes)}))
