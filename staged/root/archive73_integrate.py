from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
changes = {}

def load(name):
    p = ROOT / name
    if p not in changes:
        changes[p] = p.read_text(encoding='utf-8')
    return changes[p]

def replace(name, old, new, count=1):
    s = load(name)
    if s.count(old) != count:
        raise RuntimeError(f'{name}: expected {count} exact matches, got {s.count(old)}: {old[:100]}')
    changes[ROOT / name] = s.replace(old, new)

policy = (ROOT / 'staged/archive/archive_policy.hpp').read_text(encoding='utf-8')
policy = policy.replace('// UNAPPLIED proposal. No production target includes this header.',
    '// Stock presentation/killcam archive policy; authoritative simulation and transport remain 1 ms.')
policy = policy.replace('khz_archive_proposal', 'khz_archive')
changes[ROOT / 'native/archive_history.hpp'] = policy
header = (ROOT / 'staged/archive/native_adapter.h').read_text(encoding='utf-8')
header = header.replace('// UNAPPLIED: place alongside archive_policy.hpp only after native review.\n// This header is not included by any deployed target.',
    '// Source-owned bounded metadata for the stock historical snapshot codec.')
header = header.replace('"archive_policy.hpp"', '<archive_history.hpp>').replace('khz_archive_proposal', 'khz_archive')
changes[ROOT / 'engine/src/khz/khz_archive.h'] = header
adapter = (ROOT / 'staged/archive/native_adapter.cpp').read_text(encoding='utf-8')
adapter = adapter.replace('// UNAPPLIED native adapter proposal. No CMake entry or deployed include exists.\n// See native_integration.md for exact required hooks and proof limitations.',
    '// Stock 20 Hz archive, independent of the custom 1 kHz gameplay/network clocks.')
adapter = adapter.replace('"native_adapter.h"', '"khz_archive.h"').replace('khz_archive_proposal', 'khz_archive')
changes[ROOT / 'engine/src/khz/khz_archive.cpp'] = adapter

for name in ('engine/src/server_mp/sv_archive_mp.cpp', 'engine/src/server_mp/sv_snapshot_mp.cpp',
             'engine/src/server_mp/sv_init_mp.cpp'):
    replace(name, '#include <universal/q_shared.h>', '#include <universal/q_shared.h>\n#ifdef COD4X1KHZ\n#include <khz/khz_archive.h>\n#endif')

a = 'engine/src/server_mp/sv_archive_mp.cpp'
replace(a, '        if (v36->archivedFrame >= svsHeader.nextArchivedSnapshotFrames - svsHeader.fps && !v36->usesDelta)',
    '        if (\n#ifdef COD4X1KHZ\n            KHZ_ArchiveCanUseDeltaBase(v36)\n#else\n            v36->archivedFrame >= svsHeader.nextArchivedSnapshotFrames - svsHeader.fps && !v36->usesDelta\n#endif\n            )')
replace(a, '                MSG_WriteBit0(msg);\n                MSG_WriteLong(msg, v36->archivedFrame);',
    '                #ifdef COD4X1KHZ\n                KHZ_ArchiveEncodedDelta(v36);\n                #endif\n                MSG_WriteBit0(msg);\n                MSG_WriteLong(msg, v36->archivedFrame);')
replace(a, '    MSG_WriteBit1(msg);\n    MSG_WriteLong(msg, svsHeader.time);',
    '#ifdef COD4X1KHZ\n    KHZ_ArchiveEncodedFull();\n#endif\n    MSG_WriteBit1(msg);\n    MSG_WriteLong(msg, svsHeader.time);')

s = 'engine/src/server_mp/sv_snapshot_mp.cpp'
replace(s, 'char __cdecl SV_GetClientPositionAtTime(int client, int gametime, float *pos)\n{',
    'char __cdecl SV_GetClientPositionAtTime(int client, int gametime, float *pos)\n{\n#ifdef COD4X1KHZ\n    return KHZ_ArchiveClientPosition(client, gametime, pos);\n#else')
replace(s, '    Vec3Lerp(start, end, progress, pos);\n    return 1;\n}',
    '    Vec3Lerp(start, end, progress, pos);\n    return 1;\n#endif\n}')
replace(s, 'cachedSnapshot_t *__cdecl SV_GetCachedSnapshot(int *pArchiveTime)\n{',
    'cachedSnapshot_t *__cdecl SV_GetCachedSnapshot(int *pArchiveTime)\n{\n#ifdef COD4X1KHZ\n    return KHZ_ArchiveLookup(pArchiveTime);\n#else')
replace(s, '    *pArchiveTime = 0;\n    return 0;\n}\n\ncachedSnapshot_t *__cdecl SV_GetCachedSnapshotInternal',
    '    *pArchiveTime = 0;\n    return 0;\n#endif\n}\n\ncachedSnapshot_t *__cdecl SV_GetCachedSnapshotInternal')
replace(s, '    if (svs.archivedSnapshotFrames[archivedFrame % 1200].start < svs.nextArchivedSnapshotBuffer - 0x2000000)\n        return 0;',
    '#ifdef COD4X1KHZ\n    if (!KHZ_ArchiveCommittedDescriptor(archivedFrame)) return nullptr;\n    int previousClientIndex = -1, previousEntityIndex = -1;\n#endif\n    if (svs.archivedSnapshotFrames[archivedFrame % 1200].start < svs.nextArchivedSnapshotBuffer - 0x2000000)\n        return 0;')
replace(s, '        if (cachedFrame->archivedFrame == archivedFrame)\n        {',
    '        if (cachedFrame->archivedFrame == archivedFrame)\n        {\n#ifdef COD4X1KHZ\n            if (!KHZ_ArchiveCachedFrameMatches(cachedFrame)) continue;\n#endif')
replace(s, '*(_DWORD *)&svs.archivedSnapshotBuffer[8 * (archivedFrame % 1200) - 9596]',
    'svs.archivedSnapshotFrames[archivedFrame % 1200].size', count=3)
replace(s, '    if (MSG_ReadBit(&msg))\n    {\n        cachedFrame = &svs.cachedSnapshotFrames[svs.nextCachedSnapshotFrames % 512];',
    '    const int archiveIsFull = MSG_ReadBit(&msg);\n#ifdef COD4X1KHZ\n    if (msg.overflowed || !KHZ_ArchiveDecodedKindMatches(archivedFrame, !archiveIsFull))\n        Com_Error(ERR_DROP, "Archive committed message kind is invalid");\n#endif\n    if (archiveIsFull)\n    {\n#ifdef COD4X1KHZ\n        v6 = MSG_ReadLong(&msg);\n        if (msg.overflowed || !KHZ_ArchiveDecodedTimeMatches(archivedFrame, v6))\n            Com_Error(ERR_DROP, "Archive full message timestamp is invalid");\n#endif\n        cachedFrame = &svs.cachedSnapshotFrames[svs.nextCachedSnapshotFrames % 512];')
replace(s, '        v6 = MSG_ReadLong(&msg);\n        cachedFrame->time = v6;',
    '#ifndef COD4X1KHZ\n        v6 = MSG_ReadLong(&msg);\n#endif\n        cachedFrame->time = v6;')
replace(s, '        oldArchivedFrame = MSG_ReadLong(&msg);\n        if (oldArchivedFrame < svs.nextArchivedSnapshotFrames - 1200)',
    '        oldArchivedFrame = MSG_ReadLong(&msg);\n#ifdef COD4X1KHZ\n        v3 = MSG_ReadLong(&msg);\n        if (msg.overflowed || !KHZ_ArchiveDeltaReferenceMatches(archivedFrame, oldArchivedFrame) ||\n            !KHZ_ArchiveDecodedTimeMatches(archivedFrame, v3))\n            Com_Error(ERR_DROP, "Archive delta base or timestamp is invalid");\n#endif\n        if (oldArchivedFrame < svs.nextArchivedSnapshotFrames - 1200)')
replace(s, '        if (!oldCachedFrame)\n            return 0;\n        if (oldCachedFrame->usesDelta)',
    '        if (!oldCachedFrame)\n            return 0;\n#ifdef COD4X1KHZ\n        if (!KHZ_ArchiveCachedFrameMatches(oldCachedFrame) || oldCachedFrame->usesDelta)\n            Com_Error(ERR_DROP, "Archive delta base has invalid borrowed cache storage");\n#endif\n        if (oldCachedFrame->usesDelta)')
replace(s, '        v3 = MSG_ReadLong(&msg);\n        cachedFrame->time = v3;',
    '#ifndef COD4X1KHZ\n        v3 = MSG_ReadLong(&msg);\n#endif\n        cachedFrame->time = v3;')
replace(s, '            cachedClient = &svs.cachedSnapshotClients[svs.nextCachedSnapshotClients % 4096];\n            MSG_ReadDeltaClient(&msg, cachedFrame->time, 0, &cachedClient->cs, newnumb);',
    '#ifdef COD4X1KHZ\n            if (cachedFrame->num_clients >= 64 || newnumb >= 64 || int(newnumb) <= previousClientIndex)\n                Com_Error(ERR_DROP, "Archive full client list is invalid");\n            previousClientIndex = int(newnumb);\n#endif\n            cachedClient = &svs.cachedSnapshotClients[svs.nextCachedSnapshotClients % 4096];\n            MSG_ReadDeltaClient(&msg, cachedFrame->time, 0, &cachedClient->cs, newnumb);')
replace(s, '            while (oldnum < newnum)\n            {',
    '#ifdef COD4X1KHZ\n            if (cachedFrame->num_clients >= 64 || newnum >= 64 || newnum <= previousClientIndex)\n                Com_Error(ERR_DROP, "Archive delta client list is invalid");\n            previousClientIndex = newnum;\n#endif\n            while (oldnum < newnum)\n            {')
for variable in ('newnumc', 'newnuma'):
    replace(s, f'            MSG_ReadDeltaArchivedEntity(\n                &msg,\n                cachedFrame->time,\n                &sv.svEntities[{variable}].baseline,',
        f'#ifdef COD4X1KHZ\n            if ({variable} >= ENTITYNUM_NONE || int({variable}) <= previousEntityIndex)\n                Com_Error(ERR_DROP, "Archive entity list is invalid");\n            previousEntityIndex = int({variable});\n#endif\n            MSG_ReadDeltaArchivedEntity(\n                &msg,\n                cachedFrame->time,\n                &sv.svEntities[{variable}].baseline,')
replace(s, '    if (cachedFrame->num_entities >= 0x4000)',
    '#ifdef COD4X1KHZ\n    if (msg.overflowed || !KHZ_ArchiveCachedFrameMatches(cachedFrame))\n        Com_Error(ERR_DROP, "Archive decoded message or cache identity is invalid");\n#endif\n    if (cachedFrame->num_entities >= 0x4000)')
start = load(s).index('    SV_DisablePacketData();\n    g_archivingSnapshot = 1;')
end = load(s).index('    g_archivingSnapshot = 0;', start)
old = load(s)[start:end]
new = '''    SV_DisablePacketData();
#ifdef COD4X1KHZ
    const bool archiveDue = sv.state == SS_GAME && KHZ_ArchiveSampleDue();
    g_archivingSnapshot = archiveDue;
    if (archiveDue)
    {
        MSG_Init(&g_archiveMsg, tempServerMsgBuf, 0x20000);
        SV_ArchiveSnapshot(&g_archiveMsg);
    }
    // Close every dispatch's header, even on the 49 ticks without archive work.
    SV_GetServerStaticHeader();
    if (archiveDue) KHZ_ArchiveCompleteMessage(&g_archiveMsg);
#else
''' + old.removeprefix('    SV_DisablePacketData();\n') + '#endif\n'
replace(s, old, new)

i = 'engine/src/server_mp/sv_init_mp.cpp'
replace(i, '    svs.nextCachedSnapshotFrames = 0;\n}',
    '    svs.nextCachedSnapshotFrames = 0;\n#ifdef COD4X1KHZ\n    KHZ_ArchiveResetPolicy();\n#endif\n}')
replace(i, '        memset(&svs, 0, sizeof(svs));\n        bgs = 0;',
    '        memset(&svs, 0, sizeof(svs));\n        bgs = 0;\n#ifdef COD4X1KHZ\n        KHZ_ArchiveShutdownComplete();\n#endif')
replace('CMakeLists.txt', '      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_server_metrics.cpp"',
    '      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_server_metrics.cpp"\n      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_archive.cpp"')

for p, s in changes.items():
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(s, encoding='utf-8', newline='\n')
print(f'Applied coherent archive adaptation to {len(changes)} source files; native validation pending.')
