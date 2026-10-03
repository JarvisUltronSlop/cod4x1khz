#include "khz_client_capture.h"
#include "khz_canonical.h"
#include "khz_client_owner.h"
#include "khz_client_pose.h"
#include "khz_client_visibility.h"
#include "khz_client_input.h"
#include "khz_runtime.h"
#include "khz_snapshot_headers.h"
#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <aim_assist/aim_assist.h>
#include <gameplay_rng.hpp>
#include <cstdio>
#include <initializer_list>

// Narrow domain observations supplied at the next owner/input checkpoint.
// These read actual private state, not presentation publications.
bool KHZ_ClientPoseCanonical(khz::canonical::Hash& hash);
bool KHZ_ClientVisibilityCanonical(khz::canonical::Hash& hash);
khz::GameplayRandomState KHZ_GetGameplayRandomState();

namespace {
constexpr unsigned CaptureCapacity = 4096;
khz::capture::Record records[CaptureCapacity]; // raw BSS, no aggregate CRT temporary
khz::capture::Journal<CaptureCapacity> journal(records);
std::uint64_t capturedSession, capturedGeneration;

void put(khz::capture::Record& record, khz::capture::Domain domain, const khz::canonical::Hash& hash, bool complete = true) {
    const auto index = static_cast<unsigned>(domain); record.domains[index] = hash.finish();
    if (complete) record.domainMask |= 1u << index;
}
const char* failureName(khz::capture::Failure failure) {
    switch (failure) {
    case khz::capture::Failure::None: return "none";
    case khz::capture::Failure::MissingDomain: return "missing_private_domain";
    case khz::capture::Failure::InvalidCanonicalField: return "invalid_native_field_or_pool";
    case khz::capture::Failure::Overflow: return "bounded_capture_overflow";
    case khz::capture::Failure::Lifecycle: return "owner_lifecycle_changed";
    case khz::capture::Failure::EndedEarly: return "ended_before_target";
    }
    return "unknown";
}
}

bool KHZ_ClientCaptureBegin(const khz::capture::Request& request) {
    iassert(Sys_IsMainThread());
    if (!KHZ_ClientOwnerRunning() || !KHZ_ClientOwnerControlsActive()) return false;
    if (!journal.begin(request)) return false;
    capturedSession = capturedGeneration = 0; return true;
}
bool KHZ_ClientCaptureEnd() {
    iassert(Sys_IsMainThread());
    if (KHZ_ClientOwnerRunning() && !KHZ_ClientOwnerControlsActive()) return false;
    journal.end(); return true;
}
bool KHZ_ClientCapturePop(khz::capture::Record* record) {
    iassert(Sys_IsMainThread()); return record && journal.pop(*record);
}
void KHZ_ClientCaptureInvalidateLifecycle() { journal.invalidateLifecycle(); }
void KHZ_ClientCaptureTick(int currentMessage, int nextMessage) {
    iassert(KHZ_IsClientSimulationThread());
    if (!journal.active()) return;
    if (!KHZ_ClientOwnerRunning() || KHZ_ClientOwnerNeedsMainThread()) { journal.invalidateLifecycle(); return; }
    const auto& game = *CG_GetLocalClientGlobals(0); const auto& client = *CL_GetLocalClientGlobals(0);
    if (!game.snap || !game.nextSnap) { journal.invalidateLifecycle(); return; }
    khz::capture::Record record{};
    record.session = KHZ_AcceptedClientSession(); record.generation = KHZ_ClientOwnerGeneration();
    record.clientTick = KHZ_ClientOwnerTickNumber(); record.currentMessage = currentMessage; record.nextMessage = nextMessage;
    khz::WireHeader currentHeader{}, nextHeader{};
    const bool headersComplete = KHZ_CommittedSnapshotHeader(currentMessage, &currentHeader) &&
        KHZ_CommittedSnapshotHeader(nextMessage, &nextHeader) && currentHeader.session == record.session && nextHeader.session == record.session;
    record.currentAuthoritativeTick = currentHeader.tick; record.currentInputAck = currentHeader.input; record.currentSnapshotAck = currentHeader.ack;
    record.authoritativeTick = nextHeader.tick; record.inputAck = nextHeader.input; record.nextSnapshotAck = nextHeader.ack;
    record.commandNumber = client.cmdNumber; record.commandTime = game.predictedPlayerState.commandTime;
    record.acceptedSnapshot = client.snap.messageNum; record.acceptedServerTime = client.snap.serverTime;
    if (!capturedSession) { capturedSession = record.session; capturedGeneration = record.generation; }
    if (!record.session || capturedSession != record.session || capturedGeneration != record.generation) { journal.invalidateLifecycle(); return; }
    khz::canonical::Hash player;
    KHZ_CanonicalPlayer(player, game.predictedPlayerState); put(record, khz::capture::Domain::Player, player);
    khz::canonical::Hash input;
    KHZ_CanonicalClientInput(input, client); KHZ_CanonicalClientFeedback(input, game);
    usercmd_s command{};
    const bool commandAvailable = CL_GetUserCmd(0, client.cmdNumber, &command) != 0;
    if (commandAvailable) KHZ_CanonicalCommand(input, command);
    const bool inputComplete = KHZ_ClientInputCanonical(input);
    put(record, khz::capture::Domain::InputFeedback, input, commandAvailable && inputComplete);
    khz::canonical::Hash world;
    world.u64(currentHeader.tick); world.u64(currentHeader.input); world.u64(currentHeader.ack);
    world.u64(nextHeader.tick); world.u64(nextHeader.input); world.u64(nextHeader.ack);
    for (const auto* snapshot : {game.snap, game.nextSnap}) {
        world.scalar(snapshot->snapFlags); world.scalar(snapshot->serverTime); world.scalar(snapshot->serverCommandSequence);
        KHZ_CanonicalPlayer(world, snapshot->ps); world.scalar(snapshot->numEntities); world.scalar(snapshot->numClients);
        if (snapshot->numEntities < 0 || snapshot->numEntities > 512 || snapshot->numClients < 0 || snapshot->numClients > 64) { world.reject(); break; }
        for (int i = 0; i < snapshot->numEntities; ++i) KHZ_CanonicalEntity(world, snapshot->entities[i]);
        for (int i = 0; i < snapshot->numClients; ++i) KHZ_CanonicalClient(world, snapshot->clients[i]);
    }
    put(record, khz::capture::Domain::AcceptedWorld, world, headersComplete);
    khz::canonical::Hash pose; const bool poseComplete = KHZ_ClientPoseCanonical(pose);
    put(record, khz::capture::Domain::ControllerPoseAnimation, pose, poseComplete);
    khz::canonical::Hash aim; KHZ_CanonicalAim(aim, *AimAssist_GetGlobals(0));
    AimTarget* targets = nullptr; int targetCount = 0; AimTarget_GetClientTargetList(0, &targets, &targetCount);
    aim.scalar(targetCount);
    if (targetCount < 0 || targetCount > 64 || (targetCount && !targets)) aim.reject();
    else for (int i = 0; i < targetCount; ++i) {
        aim.scalar(targets[i].entIndex); aim.scalar(targets[i].worldDistSqr); aim.array(targets[i].mins);
        aim.array(targets[i].maxs); aim.array(targets[i].velocity);
    }
    put(record, khz::capture::Domain::Aim, aim);
    khz::canonical::Hash effects; const bool effectsComplete = KHZ_ClientVisibilityCanonical(effects);
    put(record, khz::capture::Domain::GameplayFX, effects, effectsComplete);
    khz::canonical::Hash rng; const auto state = KHZ_GetGameplayRandomState(); rng.scalar(state.crt); rng.scalar(state.hold);
    put(record, khz::capture::Domain::RNG, rng);
    // Overflow invalidates this capture and leaves gameplay running. A render
    // stall does not park the owner to save a diagnostic output queue.
    journal.push(record);
}
const char* KHZ_ClientCaptureStatusJSON() {
    iassert(Sys_IsMainThread()); const auto status = journal.status(); static char output[1536];
    std::snprintf(output, sizeof(output),
        "{\"schema\":1,\"fixture\":%llu,\"target\":%u,\"recorded\":%llu,\"drained\":%llu,\"queued\":%u,\"invalid\":%llu,\"overflows\":%llu,\"active\":%s,\"finished\":%s,\"complete\":%s,\"comparison_eligible\":%s,\"verified_restore_conditions\":%u,\"required_restore_conditions\":%u,\"required_domains\":%u,\"failure\":\"%s\",\"scope\":\"native private owner observations; authoritative input, world/script/RNG restore and actual renderer comparison require independently verified harness evidence\"}",
        status.request.fixture, status.request.samples, status.recorded, status.drained, status.queued, status.invalid, status.overflow,
        status.active ? "true" : "false", status.finished ? "true" : "false", status.complete ? "true" : "false", status.comparisonEligible ? "true" : "false",
        status.request.verifiedRestoreConditions, khz::capture::AllRestoreConditions, khz::capture::AllDomains, failureName(status.failure));
    return output;
}
