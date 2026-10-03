namespace baseline {

using namespace dependency;

void KHZ_ServerPhysicsDrainGuardRefusals() {}

class KHZ_ServerPhysicsEntry {
public:
    explicit KHZ_ServerPhysicsEntry(khz::server_physics_guard::Site);
    ~KHZ_ServerPhysicsEntry();
    KHZ_ServerPhysicsEntry(const KHZ_ServerPhysicsEntry&) = delete;
    KHZ_ServerPhysicsEntry& operator=(const KHZ_ServerPhysicsEntry&) = delete;
    bool allowed() const { return entered_; }
private:
    bool entered_;
};


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
    : entered_(khz_server_physics_fence_detail::entry(unsigned(site))) {}
KHZ_ServerPhysicsEntry::~KHZ_ServerPhysicsEntry() {
    if (entered_) khz_server_physics_fence_detail::gate.fetch_sub(1, std::memory_order_seq_cst);
}


}

namespace proposed {

using namespace dependency;

void KHZ_ServerPhysicsDrainGuardRefusals() {}

class KHZ_ServerPhysicsEntry {
public:
    explicit KHZ_ServerPhysicsEntry(khz::server_physics_guard::Site);
    ~KHZ_ServerPhysicsEntry();
    KHZ_ServerPhysicsEntry(const KHZ_ServerPhysicsEntry&) = delete;
    KHZ_ServerPhysicsEntry& operator=(const KHZ_ServerPhysicsEntry&) = delete;
    bool allowed() const { return entered_; }
private:
    bool entered_;
    std::uint64_t epoch_;
};
// Only immediately before native Com_Error takes its ERR_JMP path.
// Normal-return notices keep full-function leases intact.
void KHZ_ServerPhysicsAbortMainEntryLeases();


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


}