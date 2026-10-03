#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <setjmp.h>
#include <thread>

namespace khz::server_physics_guard {
enum class Site : unsigned { First, Second, Third };
constexpr unsigned Count = 3;
constexpr const char* Names[] = { "first", "second", "third" };
}
namespace khz::server_native_checkpoint {}
namespace dependency {
struct KHZ_ServerCheckpointFrame { unsigned fixture; };
thread_local unsigned threadId = 1;
unsigned GetCurrentThreadId() { return threadId; }
bool Sys_IsMainThread() { return threadId == 1; }
}
#include "native_snippets.hpp"

namespace {
unsigned checks = 0, failures = 0;
void require(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
}
using Site = khz::server_physics_guard::Site;
void baselineNegative() {
    using namespace baseline::khz_server_physics_fence_detail;
    // Placement ownership deliberately models skipped destructors without
    // invoking undefined C++ longjmp across automatic nontrivial objects.
    alignas(baseline::KHZ_ServerPhysicsEntry) unsigned char storage[sizeof(baseline::KHZ_ServerPhysicsEntry)];
    auto* lease = new(storage) baseline::KHZ_ServerPhysicsEntry(Site::First);
    require(lease->allowed() && gate.load() == 1, "baseline real admission");
    unsigned expected = 0;
    require(!gate.compare_exchange_strong(expected, Closed), "baseline abandoned error lease blocks actual exclusive CAS");
    lease->~KHZ_ServerPhysicsEntry();
    require(gate.load() == 0, "baseline test explicitly retires its control lease");
}
void nativeUnwindJump(proposed::KHZ_ServerPhysicsEntry* first, proposed::KHZ_ServerPhysicsEntry* second, jmp_buf* jump) {
    new(first) proposed::KHZ_ServerPhysicsEntry(Site::First);
    new(second) proposed::KHZ_ServerPhysicsEntry(Site::Second);
    proposed::KHZ_ServerPhysicsAbortMainEntryLeases();
    longjmp(*jump, 1);
}
void unwindCase() {
    using namespace proposed::khz_server_physics_fence_detail;
    alignas(proposed::KHZ_ServerPhysicsEntry) unsigned char firstBytes[sizeof(proposed::KHZ_ServerPhysicsEntry)], secondBytes[sizeof(proposed::KHZ_ServerPhysicsEntry)];
    auto* first = reinterpret_cast<proposed::KHZ_ServerPhysicsEntry*>(firstBytes);
    auto* second = reinterpret_cast<proposed::KHZ_ServerPhysicsEntry*>(secondBytes);
    jmp_buf jump;
    if (setjmp(jump) == 0) nativeUnwindJump(first, second, &jump);
    require(gate.load() == 0 && nativeEntryDepth == 0 && abandonedErrorEntries.load() == 2, "two real admissions abandoned at native jump seam");
    require(pendingReason.load() == khz::server_physics_guard::Count + 1, "error refusal retained outside gameplay restore state");
    const auto epoch = nativeEntryEpoch;
    proposed::KHZ_ServerPhysicsAbortMainEntryLeases();
    require(nativeEntryEpoch == epoch && gate.load() == 0, "duplicate cleanup idle and stable");
    {
        proposed::KHZ_ServerPhysicsEntry newLease(Site::Third);
        require(newLease.allowed() && gate.load() == 1 && nativeEntryDepth == 1, "new generation admitted after abandoned error");
        first->~KHZ_ServerPhysicsEntry(); second->~KHZ_ServerPhysicsEntry();
        require(gate.load() == 1 && nativeEntryDepth == 1, "old destructors cannot release new generation");
    }
    require(gate.load() == 0 && nativeEntryDepth == 0, "new lease normal retirement");
    unsigned expected = 0;
    require(gate.compare_exchange_strong(expected, Closed), "exclusive CAS possible after actual error unwind");
    proposed::KHZ_ServerPhysicsAbortMainEntryLeases();
    require(gate.load() == Closed, "idle error cleanup never opens exclusive fence");
    {
        proposed::KHZ_ServerPhysicsEntry refused(Site::First);
        require(!refused.allowed() && gate.load() == Closed && nativeEntryDepth == 0, "refused closed entry owns no lease");
    }
    gate.store(0);
}
void normalNoticeAndForeign() {
    using namespace proposed::khz_server_physics_fence_detail;
    const auto abandonedBefore = abandonedErrorEntries.load();
    {
        proposed::KHZ_ServerPhysicsEntry outer(Site::First);
        const auto epoch = nativeEntryEpoch;
        // Native ERR_SCRIPT/MAPLOAD branches which return must NOT call abort.
        { proposed::KHZ_ServerPhysicsEntry inner(Site::Second);
          require(outer.allowed() && inner.allowed() && gate.load() == 2 && nativeEntryDepth == 2, "normal nested native scopes retain complete leases"); }
        require(gate.load() == 1 && nativeEntryDepth == 1 && nativeEntryEpoch == epoch, "normal-return notice keeps outer lease");
        std::thread foreign([&] {
            dependency::threadId = 2;
            proposed::KHZ_ServerPhysicsEntry rejected(Site::Third);
            proposed::KHZ_ServerPhysicsAbortMainEntryLeases();
            require(!rejected.allowed(), "foreign entry refused before effects");
        });
        foreign.join();
        require(gate.load() == 1 && nativeEntryDepth == 1, "foreign error cannot release main native lease");
    }
    require(gate.load() == 0 && nativeEntryDepth == 0 && abandonedErrorEntries.load() == abandonedBefore, "normal scopes balanced without abandonment");
}
}
int main() {
    baselineNegative(); unwindCase(); normalNoticeAndForeign();
    std::printf("{\"passed\":%s,\"assertions\":%u,\"failures\":%u,\"baseline_negative_controls\":1,\"native_com_error_executed\":false,\"production_applied\":false}\n", failures ? "false" : "true", checks, failures);
    return failures ? 1 : 0;
}
