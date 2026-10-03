#include <universal/q_shared.h>
#include "khz_server_checkpoint.h"
#include "khz_host.h"
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <cstdio>

namespace {
using namespace khz::server_checkpoint;
Coordinator coordinator;
Ranges ranges;
KHZ_ServerCheckpointObserver observer;
bool callbackContext;
Branch branch;
bool pausedOwner() {
    return Sys_IsMainThread() && com_dedicated && com_dedicated->current.integer != 0 &&
        Dvar_GetBool("khz_benchmark") && KHZ_ServerCheckpointPaused();
}
struct Context {
    Context() { iassert(!callbackContext); callbackContext = true; }
    ~Context() { callbackContext = false; }
};
bool observe(std::uint64_t fixture, Frame* frame) {
    if (!pausedOwner()) return coordinator.reject(Failure::NotPausedOwner, "server_checkpoint_requires_actual_paused_dedicated_owner");
    if (!observer) return coordinator.reject(Failure::MissingObserver, "server_checkpoint_actual_fence_observer_missing");
    char reason[256]{};
    if (!observer(fixture, frame, reason, sizeof(reason)))
        return coordinator.reject(Failure::InvalidIdentity, reason[0] ? reason : "server_checkpoint_actual_fence_rejected");
    if (!frame || !valid(*frame) || frame->fixture != fixture)
        return coordinator.reject(Failure::InvalidIdentity, "server_checkpoint_observer_identity_invalid");
    return true;
}
const char* phaseName(Phase p) {
    switch (p) {
    case Phase::Empty: return "empty"; case Phase::Capturing: return "capturing";
    case Phase::Pinned: return "pinned"; case Phase::Validating: return "validating";
    case Phase::Restoring: return "restoring"; case Phase::Discarding: return "discarding";
    } return "invalid";
}
void escaped(const char* source, char* output, unsigned capacity) {
    unsigned at = 0;
    if (!capacity) return;
    for (; source && *source && at + 2 < capacity; ++source) {
        if (*source == '"' || *source == '\\') output[at++] = '\\';
        output[at++] = static_cast<unsigned char>(*source) < 32 ? '_' : *source;
    }
    output[at] = 0;
}
}
bool KHZ_InitServerCheckpoint(KHZ_ServerCheckpointObserver value) {
    iassert(Sys_IsMainThread());
    if (!value || observer || coordinator.pinned()) return false;
    observer = value; return true;
}
bool KHZ_ServerCheckpointRegister(const KHZ_ServerCheckpointProvider& p) {
    iassert(Sys_IsMainThread()); return coordinator.registerProvider(p);
}
bool KHZ_ServerCheckpointRequireParts(KHZ_ServerCheckpointDomain d, unsigned mask) {
    iassert(Sys_IsMainThread()); return coordinator.requireParts(d, mask);
}
bool KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain d, const void* base, std::size_t bytes) {
    iassert(Sys_IsMainThread()); return !coordinator.pinned() && ranges.add(d, base, bytes);
}
bool KHZ_ServerCheckpointReplaceMapRanges(KHZ_ServerCheckpointDomain d,
    const void* const* bases, const std::size_t* bytes, unsigned count) {
    iassert(Sys_IsMainThread());
    return !callbackContext && !coordinator.pinned() && ranges.replaceMap(d, bases, bytes, count);
}
bool KHZ_ServerCheckpointResetMapRanges() {
    iassert(Sys_IsMainThread());
    if (callbackContext || coordinator.pinned()) return false;
    ranges.resetMap(); return true;
}
bool KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain d, const void* base, std::size_t bytes) {
    return KHZ_ServerCheckpointContext() && ranges.contains(d, base, bytes);
}
bool KHZ_ServerCheckpointRestoresAddress(const void* base, std::size_t bytes) {
    return KHZ_ServerCheckpointContext() && ranges.containsAny(base, bytes);
}
bool KHZ_ServerCheckpointCapture(std::uint64_t fixture) {
    Frame current{}; if (!observe(fixture, &current)) return false;
    Context scope; const bool captured = coordinator.capture(current);
    if (captured) branch.reset();
    return captured;
}
bool KHZ_ServerCheckpointValidateRestore() {
    Frame current{}; if (!observe(coordinator.frame().fixture, &current)) return false;
    Context scope; return coordinator.validateRestore(current);
}
bool KHZ_ServerCheckpointRestore() {
    Frame current{}; if (!observe(coordinator.frame().fixture, &current)) return false;
    Context scope; return coordinator.restore(current);
}
bool KHZ_ServerCheckpointDiscard() {
    iassert(Sys_IsMainThread());
    // Discard only releases fixture images/pins, never live native state; allow
    // it after a refused restore to make a controlled shutdown possible.
    if (callbackContext) return false;
    Context scope; return coordinator.discard();
}
bool KHZ_ServerCheckpointPinned() { return coordinator.pinned(); }
bool KHZ_ServerCheckpointContext() { return callbackContext && pausedOwner(); }
bool KHZ_ServerCheckpointCanReleaseAssets() { return !coordinator.pinned(); }
void KHZ_ServerCheckpointRejectExternalEffect(const char* reason) {
    iassert(Sys_IsMainThread());
    if (!coordinator.pinned()) return;
    branch.reject(reason);
}
bool KHZ_ServerCheckpointBranchEligible() { return branch.eligible(); }
const char* KHZ_ServerCheckpointStatusJSON() {
    iassert(Sys_IsMainThread());
    static char output[3072]; char reason[512], externalReason[512];
    escaped(coordinator.reason(), reason, sizeof(reason)); escaped(branch.reason(), externalReason, sizeof(externalReason));
    const auto& f = coordinator.frame();
    std::snprintf(output, sizeof(output),
        "{\"phase\":\"%s\",\"fixture\":%llu,\"session\":%llu,\"map_generation\":%llu,"
        "\"logical_tick\":%llu,\"native_time\":%d,\"server_id\":%d,\"map_checksum\":%u,"
        "\"required_domains\":%u,\"registered_domains\":%u,\"captured_domains\":%u,"
        "\"validated_domains\":%u,\"restored_domains\":%u,\"pinned\":%s,"
        "\"native_restore_complete\":%s,\"full_world_replay_verified\":false,"
        "\"branch_eligible\":%s,\"external_effect_rejections\":%llu,\"branch_reason\":\"%s\","
        "\"storage_bytes\":%llu,\"captures\":%llu,\"validations\":%llu,\"restorations\":%llu,"
        "\"failures\":%llu,\"failure\":%u,\"reason\":\"%s\"}",
        phaseName(coordinator.phase()), f.fixture, f.session, f.mapGeneration, f.logicalTick,
        f.nativeTime, f.serverId, f.mapChecksum, AllDomains, coordinator.registeredDomains(),
        coordinator.capturedDomains(), coordinator.validatedDomains(), coordinator.restoredDomains(),
        coordinator.pinned() ? "true" : "false", coordinator.restoreComplete() ? "true" : "false",
        branch.eligible() ? "true" : "false", branch.rejections(), externalReason,
        static_cast<unsigned long long>(coordinator.storageBytes()), coordinator.captures(),
        coordinator.validations(), coordinator.restorations(), coordinator.failures(), unsigned(coordinator.failure()), reason);
    return output;
}
