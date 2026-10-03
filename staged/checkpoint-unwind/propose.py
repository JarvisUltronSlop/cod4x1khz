"""Prepare a source-bound Physics lease error-unwind proposal; never apply.

The checkpoint opt-in remains OFF. Native errors which return normally must
retain their leases; only the actual ERR_JMP branch abandons them.
"""
from pathlib import Path
from datetime import datetime, timezone
import difflib
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
STAGE = Path(__file__).resolve().parent
FILES = ('engine/src/khz/khz_server_physics_guards.h',
         'engine/src/khz/khz_server_physics_fence.inl', 'engine/src/qcommon/common.cpp')

def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Stale or ambiguous native anchor: ' + before[:100])
    return text.replace(before, after, 1)

def changes():
    source = {name: (ROOT / name).read_text(encoding='utf8') for name in FILES}
    after = dict(source)
    after[FILES[0]] = once(after[FILES[0]], '    bool entered_;\n};',
        '    bool entered_;\n    std::uint64_t epoch_;\n};\n'
        '// Only immediately before native Com_Error takes its ERR_JMP path.\n'
        '// Normal-return notices keep full-function leases intact.\n'
        'void KHZ_ServerPhysicsAbortMainEntryLeases();')
    after[FILES[1]] = once(after[FILES[1]],
        'KHZ_ServerCheckpointFrame fenceFrame; // Owner only; never read by foreign code.',
        'KHZ_ServerCheckpointFrame fenceFrame; // Owner only; never read by foreign code.\n'
        'thread_local unsigned nativeEntryDepth = 0;\n'
        'thread_local std::uint64_t nativeEntryEpoch = 1;\n'
        'std::atomic<std::uint64_t> abandonedErrorEntries{0};')
    original = '''KHZ_ServerPhysicsEntry::KHZ_ServerPhysicsEntry(khz::server_physics_guard::Site site)
    : entered_(khz_server_physics_fence_detail::entry(unsigned(site))) {}
KHZ_ServerPhysicsEntry::~KHZ_ServerPhysicsEntry() {
    if (entered_) khz_server_physics_fence_detail::gate.fetch_sub(1, std::memory_order_seq_cst);
}'''
    replacement = '''KHZ_ServerPhysicsEntry::KHZ_ServerPhysicsEntry(khz::server_physics_guard::Site site)
    : entered_(khz_server_physics_fence_detail::entry(unsigned(site))),
      epoch_(khz_server_physics_fence_detail::nativeEntryEpoch) {
    if (entered_) ++khz_server_physics_fence_detail::nativeEntryDepth;
}
KHZ_ServerPhysicsEntry::~KHZ_ServerPhysicsEntry() {
    using namespace khz_server_physics_fence_detail;
    // A native longjmp may skip destructors. An old lease must never release
    // entries admitted later, even if platform unwinding invokes it afterward.
    if (!entered_ || epoch_ != nativeEntryEpoch) return;
    if (!owner() || !nativeEntryDepth) std::abort();
    --nativeEntryDepth;
    const auto before = gate.fetch_sub(1, std::memory_order_seq_cst);
    if ((before & Closed) || !(before & ActiveMask)) std::abort();
}
void KHZ_ServerPhysicsAbortMainEntryLeases() {
    using namespace khz_server_physics_fence_detail;
    // Foreign native entries are refused before admission and may not access
    // main coordinator state. Main is the sole admitted Physics owner.
    if (!owner() || !Sys_IsMainThread() || !nativeEntryDepth) return;
    unsigned expected = nativeEntryDepth;
    if (nativeEntryEpoch == UINT64_MAX ||
        !gate.compare_exchange_strong(expected, 0, std::memory_order_seq_cst,
                                      std::memory_order_seq_cst)) std::abort();
    abandonedErrorEntries.fetch_add(nativeEntryDepth, std::memory_order_seq_cst);
    nativeEntryDepth = 0;
    ++nativeEntryEpoch;
    recordRefusal(Count, false);
}'''
    after[FILES[1]] = once(after[FILES[1]], original, replacement)
    # The include is guarded consistently with the actual native entry sites.
    guard = 'defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(KISAK_DEDICATED)'
    after[FILES[2]] = '#if ' + guard + '\n#include <khz/khz_server_physics_guards.h>\n#endif\n' + after[FILES[2]]
    after[FILES[2]] = once(after[FILES[2]],
        '        Value = (jmp_buf *)Sys_GetValue(2);\n        longjmp(*Value, -1);',
        '        Value = (jmp_buf *)Sys_GetValue(2);\n#if ' + guard + '\n'
        '        KHZ_ServerPhysicsAbortMainEntryLeases();\n#endif\n        longjmp(*Value, -1);')
    return source, after

def main():
    before, after = changes()
    output = STAGE / 'prepared'
    output.mkdir(exist_ok=True)
    patch = []
    for name in FILES:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(after[name], encoding='utf8', newline='\n')
        patch.extend(difflib.unified_diff(before[name].splitlines(True), after[name].splitlines(True), fromfile='a/' + name, tofile='b/' + name))
    (STAGE / 'proposal.patch').write_text(''.join(patch), encoding='utf8')
    report = {'schema': 1, 'prepared_utc': datetime.now(timezone.utc).isoformat(),
              'applied': False, 'compiled': False, 'native_error_attestation': False,
              'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in FILES},
              'prepared_sha256': {name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in FILES},
              'scope': 'Opt-in Physics entry leases only; animation/asset admission and full native checkpoint integration remain separate'}
    (STAGE / 'proposal.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'prepared': True, 'applied': False, 'native_files': len(FILES)}))

if __name__ == '__main__':
    main()
