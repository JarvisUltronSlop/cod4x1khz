"""Apply source-bound, disabled-by-default native render diagnostic hooks."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
record = json.loads((ROOT / 'results/build75-source-and-binary-hashes.json').read_text(encoding='utf-8-sig'))
changes = {}
def replace(name, before, after):
    text = changes.get(name, (ROOT / name).read_text(encoding='utf-8'))
    if text.count(before) != 1:
        raise ValueError('Missing/ambiguous native hook in ' + name + ': ' + before[:90])
    changes[name] = text.replace(before, after, 1)
def include(name):
    text = (ROOT / name).read_text(encoding='utf-8')
    changes[name] = '#ifdef COD4X1KHZ\n#include <khz/khz_render_profile.h>\n#endif\n' + text

replace('CMakeLists.txt', '  set_target_properties(KisakCOD-dedi PROPERTIES ENABLE_EXPORTS ON)',
    '  target_sources(KisakCOD-mp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_render_profile.cpp")\n'
    '  set_target_properties(KisakCOD-dedi PROPERTIES ENABLE_EXPORTS ON)')
files = ('engine/src/khz/khz_runtime.cpp', 'engine/src/khz/khz_ui.cpp',
         'engine/src/qcommon/threads.cpp', 'engine/src/gfx_d3d/r_rendercmds.cpp',
         'engine/src/gfx_d3d/r_workercmds.cpp', 'engine/src/gfx_d3d/rb_backend.cpp',
         'engine/src/gfx_d3d/r_init.cpp')
for name in files: include(name)
runtime = files[0]
replace(runtime, '    resetStats();\n#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)',
    '    resetStats();\n    KHZ_RenderProfileInit();\n#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)')
replace(runtime, '    if (KHZ_ClientOwnerRunning()) KHZ_ClientOwnerConsumePresentation();\n    {\n        KHZ_TimingScope frontendTimer',
    '    if (KHZ_ClientOwnerRunning()) {\n        KHZ_RenderProfileScope consumeTrace(KHZ_RenderStage::Consume);\n        KHZ_ClientOwnerConsumePresentation();\n    }\n    {\n        KHZ_RenderProfileScope frontendTrace(KHZ_RenderStage::Frontend);\n        KHZ_TimingScope frontendTimer')
replace(files[1], '    KHZ_CommandControlScope controls;\n    if (!I_strnicmp(action, "inspect ", 8)',
    '    if (!I_strnicmp(action, "renderprofile ", 14)) return KHZ_RenderProfileProbe(action + 14);\n'
    '    KHZ_CommandControlScope controls;\n    if (!I_strnicmp(action, "inspect ", 8)')
replace(files[2], 'void __cdecl Sys_WaitForWorkerCmd()\n{',
    'void __cdecl Sys_WaitForWorkerCmd()\n{\n#ifdef COD4X1KHZ\n    KHZ_RenderProfileScope waitTrace(KHZ_RenderStage::WorkerWait);\n#endif')
replace(files[3], '        PROF_SCOPED("WaitRenderer");',
    '        PROF_SCOPED("WaitRenderer");\n#ifdef COD4X1KHZ\n        KHZ_RenderProfileScope waitTrace(KHZ_RenderStage::RendererWait);\n#endif')
replace(files[4], '    PROF_SCOPED("R_WaitFrontendWorkerCmds");',
    '    PROF_SCOPED("R_WaitFrontendWorkerCmds");\n#ifdef COD4X1KHZ\n    KHZ_RenderProfileScope waitTrace(KHZ_RenderStage::FrontendWorkers);\n#endif')
replace(files[5], 'void RB_RenderThreadIdle()\n{',
    'void RB_RenderThreadIdle()\n{\n#ifdef COD4X1KHZ\n    KHZ_RenderProfileScope idleTrace(KHZ_RenderStage::BackendIdle);\n#endif')
replace(files[5], '        KHZ_TimingScope presentTimer(KHZ_Timing::Present);',
    '        KHZ_TimingScope presentTimer(KHZ_Timing::Present);\n        KHZ_RenderProfileScope presentTrace(KHZ_RenderStage::Present);')
replace(files[5], 'void __cdecl RB_RenderCommandFrame(const GfxBackEndData *data)\n{',
    'void __cdecl RB_RenderCommandFrame(const GfxBackEndData *data)\n{\n#ifdef COD4X1KHZ\n    KHZ_RenderProfileBackendBegin(dx.device);\n#endif')
replace(files[5], '    Sys_RenderCompleted();\n    {',
    '    #ifdef COD4X1KHZ\n    KHZ_RenderProfileDrawEnd();\n    #endif\n    Sys_RenderCompleted();\n    {')
replace(files[5], '        RB_EndFrame(drawType);\n    }\n    //Profile_Begin(172);',
    '        RB_EndFrame(drawType);\n    }\n#ifdef COD4X1KHZ\n    KHZ_RenderProfileFrameEnd();\n#endif\n    //Profile_Begin(172);')
replace(files[6], '        R_SyncRenderThread();\n        rg.registered = 0;',
    '        R_SyncRenderThread();\n#ifdef COD4X1KHZ\n        KHZ_RenderProfileShutdown();\n#endif\n        rg.registered = 0;')

backup = ROOT / 'staged/render-profile/before76'
if backup.exists(): raise ValueError('Retained original bytes already exist; refuse reapply')
before_hashes = {}
for name in changes:
    raw = (ROOT / name).read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if record['source_sha256'].get(name) != digest:
        raise ValueError('Source differs from archived Build 75: ' + name)
    before_hashes[name] = digest
for name, text in changes.items():
    path = ROOT / name; original = path.read_bytes()
    retained = backup / name; retained.parent.mkdir(parents=True, exist_ok=True); retained.write_bytes(original)
    path.write_bytes(text.replace('\n', '\r\n' if b'\r\n' in original else '\n').encode('utf-8'))
output = ROOT / 'staged/render-profile/application76.json'
output.write_text(json.dumps({'applied': True, 'diagnostic_default': 'off', 'gameplay_changed': False,
    'before_sha256': before_hashes,
    'after_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in changes}}, indent=2) + '\n')
print(json.dumps({'applied': True, 'source_files': len(changes), 'result': str(output)}))
