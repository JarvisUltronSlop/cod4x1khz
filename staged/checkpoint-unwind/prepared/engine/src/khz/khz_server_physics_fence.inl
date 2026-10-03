// Source-owned implementation included by Physics part 0 only.
#include "khz_server_physics_guards.h"
#include <qcommon/threads.h>
#include <atomic>
#include <cstdlib>

namespace khz_server_physics_fence_detail {
using namespace khz::server_physics_guard;
using namespace khz::server_native_checkpoint;
constexpr unsigned Closed = 0x80000000u, ActiveMask = ~Closed, MaxEntries = 2048;
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(KISAK_DEDICATED)
constexpr bool NativeHooksCompiled = true;
#else
constexpr bool NativeHooksCompiled = false;
#endif
// One seq_cst word linearizes entry admission and exclusive closure. An
// observer CAS 0->Closed cannot pass while ANY scoped entry is admitted.
std::atomic<unsigned> gate{0}, ownerThread{0}, enrolled{0}, pendingReason{0};
std::atomic<std::uint64_t> checks[Count]{}, refusals[Count]{};
std::atomic<std::uint64_t> foreignRefusals{0}, fenceClosures{0}, fenceReleases{0};
std::atomic<std::uint64_t> unknownRefusals{0};
KHZ_ServerCheckpointFrame fenceFrame; // Owner only; never read by foreign code.
thread_local unsigned nativeEntryDepth = 0;
thread_local std::uint64_t nativeEntryEpoch = 1;
std::atomic<std::uint64_t> abandonedErrorEntries{0};

bool owner() { return ownerThread.load(std::memory_order_seq_cst) == GetCurrentThreadId(); }
void recordRefusal(unsigned site, bool foreign) {
    if (site < Count) refusals[site].fetch_add(1, std::memory_order_seq_cst);
    else unknownRefusals.fetch_add(1, std::memory_order_seq_cst);
    if (foreign) foreignRefusals.fetch_add(1, std::memory_order_seq_cst);
    unsigned empty = 0;
    pendingReason.compare_exchange_strong(empty, site < Count ? site + 1 : Count + 1,
        std::memory_order_seq_cst, std::memory_order_seq_cst);
}
const char* reasonName(unsigned encoded) {
    return encoded && encoded <= Count ? Names[encoded - 1] : "server_physics_foreign_or_unknown_native_entry";
}
bool entry(unsigned site) {
    const unsigned actual = GetCurrentThreadId();
    if (site >= Count) { recordRefusal(site, true); return false; }
    checks[site].fetch_add(1, std::memory_order_seq_cst);
    // The first native collider initialization happens in the original CRT
    // startup constructors, before WinMain creates game threads. Pin that
    // actual thread, then verify its identity at every main registration.
    unsigned expectedOwner = 0;
    ownerThread.compare_exchange_strong(expectedOwner, actual,
        std::memory_order_seq_cst, std::memory_order_seq_cst);
    if (ownerThread.load(std::memory_order_seq_cst) != actual) {
        recordRefusal(site, true); return false; // No coordinator read/call.
    }
    unsigned state = gate.load(std::memory_order_seq_cst);
    for (unsigned retry = 0; retry < 8; ++retry) {
        if ((state & Closed) || (state & ActiveMask) >= MaxEntries) {
            recordRefusal(site, false); KHZ_ServerPhysicsDrainGuardRefusals(); return false;
        }
        if (gate.compare_exchange_strong(state, state + 1,
            std::memory_order_seq_cst, std::memory_order_seq_cst)) {
            // Explicit post-admission gate check. With the packed counter an
            // exclusive CAS cannot win until this lease is released, so a
            // closed result would indicate a violated gate implementation.
            if (!(gate.load(std::memory_order_seq_cst) & Closed)) return true;
            gate.fetch_sub(1, std::memory_order_seq_cst); recordRefusal(site, false); return false;
        }
    }
    recordRefusal(site, false); KHZ_ServerPhysicsDrainGuardRefusals(); return false;
}
}

KHZ_ServerPhysicsEntry::KHZ_ServerPhysicsEntry(khz::server_physics_guard::Site site)
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
}
void KHZ_ServerPhysicsDrainGuardRefusals() {
    using namespace khz_server_physics_fence_detail;
    if (!owner() || !Sys_IsMainThread()) { recordRefusal(Count, true); return; }
    // This is the only rejection path that touches main-owned coordinator
    // fields. Before capture starts, retain the atomic pending reason so the
    // observer rejects rather than erasing an unlatched fixture failure.
    if (!KHZ_ServerCheckpointPinned()) return;
    const auto pending = pendingReason.exchange(0, std::memory_order_seq_cst);
    if (pending) KHZ_ServerCheckpointRejectExternalEffect(reasonName(pending));
}
bool KHZ_ServerPhysicsFenceEnroll(unsigned part) {
    using namespace khz_server_physics_fence_detail;
    if (!NativeHooksCompiled || part >= 8 || !Sys_IsMainThread() || gate.load(std::memory_order_seq_cst)) return false;
    unsigned expected = 0;
    ownerThread.compare_exchange_strong(expected, GetCurrentThreadId(), std::memory_order_seq_cst, std::memory_order_seq_cst);
    if (!owner()) return false;
    const auto bit = 1u << part;
    return !(enrolled.fetch_or(bit, std::memory_order_seq_cst) & bit);
}
bool KHZ_ServerPhysicsFenceEnter(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    using namespace khz_server_physics_fence_detail;
    if (!NativeHooksCompiled || !owner() || !Sys_IsMainThread() || !khz::server_checkpoint::valid(f) ||
        enrolled.load(std::memory_order_seq_cst) != 0xff)
        return reject(text, capacity, "server_physics_fence_requires_actual_owner_all8_hooks_and_identity");
    if (gate.load(std::memory_order_seq_cst) == Closed) return KHZ_ServerPhysicsFenceIdle(f, text, capacity);
    unsigned expected = 0;
    if (!gate.compare_exchange_strong(expected, Closed, std::memory_order_seq_cst, std::memory_order_seq_cst))
        return reject(text, capacity, "server_physics_native_entry_active_before_all_domain_capture");
    fenceFrame = f; fenceClosures.fetch_add(1, std::memory_order_seq_cst);
    return KHZ_ServerPhysicsFenceIdle(f, text, capacity);
}
bool KHZ_ServerPhysicsFenceCommitHeld(const KHZ_ServerCheckpointFrame& f) {
    using namespace khz_server_physics_fence_detail;
    return NativeHooksCompiled && owner() && Sys_IsMainThread() && gate.load(std::memory_order_seq_cst) == Closed &&
        enrolled.load(std::memory_order_seq_cst) == 0xff && khz::server_checkpoint::sameLifetime(fenceFrame, f);
}
void KHZ_ServerPhysicsRequireCommitFence(const KHZ_ServerCheckpointFrame& f) {
    using namespace khz_server_physics_fence_detail;
    if (KHZ_ServerPhysicsFenceCommitHeld(f)) return;
    recordRefusal(Count, !owner());
    if (!owner()) std::abort(); // No foreign coordinator or Com_Error state.
    Com_Error(ERR_FATAL, "Native Physics checkpoint commit lost its exclusive all-domain owner fence");
    std::abort();
}
bool KHZ_ServerPhysicsFenceIdle(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    using namespace khz_server_physics_fence_detail;
    if (!KHZ_ServerPhysicsFenceCommitHeld(f))
        return reject(text, capacity, "server_physics_exclusive_fence_not_closed_enrolled_or_same_lifetime");
    const auto pending = pendingReason.load(std::memory_order_seq_cst);
    if (pending) {
        // Only the real owner may consume/latch this atomic foreign refusal.
        KHZ_ServerPhysicsDrainGuardRefusals();
        return reject(text, capacity, reasonName(pending));
    }
    return true;
}
bool KHZ_ServerPhysicsFenceRelease(char* text, unsigned capacity) {
    using namespace khz_server_physics_fence_detail;
    if (!owner() || !Sys_IsMainThread())
        return reject(text, capacity, "server_physics_fence_release_not_actual_owner");
    // Root owns release only after coordinator Empty and every native provider
    // has discarded. Never reopen midway through reverse cleanup.
    if (KHZ_ServerCheckpointPinned())
        return reject(text, capacity, "server_physics_fence_release_before_all_domain_cleanup");
    if (gate.load(std::memory_order_seq_cst) != Closed)
        return reject(text, capacity, "server_physics_fence_release_not_exclusive_idle");
    // Clear the completed fixture while admission remains closed. No foreign
    // entry can acquire a lease or change this word, and the real owner is the
    // sole observer. A refusal racing AFTER this exchange remains recorded;
    // reopening must never clear a newly published foreign refusal.
    pendingReason.exchange(0, std::memory_order_seq_cst); fenceFrame = {};
    unsigned expected = Closed;
    const bool released = gate.compare_exchange_strong(expected, 0,
        std::memory_order_seq_cst, std::memory_order_seq_cst);
    if (!released) {
        Com_Error(ERR_FATAL, "Native Physics checkpoint exclusive cleanup gate changed outside its actual owner");
        std::abort();
    }
    fenceReleases.fetch_add(1, std::memory_order_seq_cst); return true;
}
bool KHZ_ServerPhysicsAllowUnsupportedMutation(const char* operation) {
    using namespace khz_server_physics_fence_detail;
    // Compatibility for direct owner control guards. Native functions use
    // full-function leases; this helper alone is not entry ownership.
    if (!owner()) { recordRefusal(Count, true); return false; }
    if (!(gate.load(std::memory_order_seq_cst) & Closed)) return true;
    recordRefusal(Count, false);
    if (KHZ_ServerCheckpointPinned()) {
        KHZ_ServerCheckpointRejectExternalEffect(operation ? operation : "server_physics_unsupported_native_mutation");
        KHZ_ServerPhysicsDrainGuardRefusals();
    }
    return false;
}
[[noreturn]] void KHZ_ServerPhysicsFatalRejection(khz::server_physics_guard::Site site) {
    using namespace khz_server_physics_fence_detail;
    // Never resume an ODE fatal assertion caller. A foreign fatal call cannot
    // invoke main-owned Com_Error/coordinator state; preserve fatal behavior.
    if (!owner()) std::abort();
    KHZ_ServerPhysicsDrainGuardRefusals();
    Com_Error(ERR_FATAL, "Pinned server checkpoint reached native ODE fatal handler %s", reasonName(unsigned(site) + 1));
    std::abort();
}
const char* KHZ_ServerPhysicsGuardStatsJSON() {
    using namespace khz_server_physics_fence_detail;
    if (!owner() || !Sys_IsMainThread()) {
        recordRefusal(Count, true);
        return "{\"complete\":false,\"reason\":\"server_physics_stats_not_actual_owner\"}";
    }
    KHZ_ServerPhysicsDrainGuardRefusals();
    static char output[49152];
    std::uint64_t totalChecks = 0, totalRefusals = unknownRefusals.load(std::memory_order_seq_cst);
    for (unsigned i = 0; i < Count; ++i) {
        totalChecks += checks[i].load(std::memory_order_seq_cst);
        totalRefusals += refusals[i].load(std::memory_order_seq_cst);
    }
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(KISAK_DEDICATED)
    const char* enabled = "true";
#else
    const char* enabled = "false";
#endif
    const auto state = gate.load(std::memory_order_seq_cst);
    int used = std::snprintf(output, sizeof(output),
        "{\"complete\":true,\"compiled_native_hooks\":%s,\"scope\":\"empty_native_ode\","
        "\"required_physics_parts\":255,\"enrolled_parts\":%u,\"guard_entries\":%u,\"checks\":%llu,\"refusals\":%llu,"
        "\"foreign_refusals\":%llu,\"unknown_refusals\":%llu,\"pending_refusal\":%u,\"active_native_entries\":%u,\"exclusive_fence\":%s,"
        "\"fence_closures\":%llu,\"fence_releases\":%llu,\"owner_thread\":%u,\"sites\":[",
        enabled, enrolled.load(std::memory_order_seq_cst), Count, totalChecks, totalRefusals,
        foreignRefusals.load(std::memory_order_seq_cst), unknownRefusals.load(std::memory_order_seq_cst), pendingReason.load(std::memory_order_seq_cst),
        state & ActiveMask, (state & Closed) ? "true" : "false", fenceClosures.load(std::memory_order_seq_cst),
        fenceReleases.load(std::memory_order_seq_cst), ownerThread.load(std::memory_order_seq_cst));
    for (unsigned i = 0; i < Count; ++i) {
        if (used < 0 || unsigned(used) >= sizeof(output)) break;
        const int n = std::snprintf(output + used, sizeof(output) - unsigned(used),
            "%s{\"id\":%u,\"name\":\"%s\",\"checks\":%llu,\"refusals\":%llu}", i ? "," : "", i, Names[i],
            checks[i].load(std::memory_order_seq_cst), refusals[i].load(std::memory_order_seq_cst));
        if (n < 0 || unsigned(n) >= sizeof(output) - unsigned(used)) { used = -1; break; }
        used += n;
    }
    if (used < 0 || unsigned(used) >= sizeof(output) - 3)
        std::snprintf(output, sizeof(output), "{\"complete\":false,\"guard_entries\":%u}", Count);
    else std::memcpy(output + used, "]}", 3);
    return output;
}
