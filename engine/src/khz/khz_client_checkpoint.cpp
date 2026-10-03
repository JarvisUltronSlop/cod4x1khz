#include "khz_client_checkpoint.h"
#include "khz_client_owner.h"
#include "khz_input_owner.h"
#include "khz_canonical.h"
#include "khz_client_body.h"
#include "khz_client_input.h"
#include "khz_snapshot_headers.h"
#include <universal/com_math.h>
#include <replay_capture.hpp>
#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <aim_assist/aim_assist.h>
#include <fp_environment.hpp>
#include <core.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

void KHZ_ClientPoseRegisterCheckpoint();
void KHZ_XAnimOwnerRegisterCheckpoint();
void KHZ_ClientAimTargetsRegisterCheckpoint();
void KHZ_ClientProjectileRegisterCheckpoint();
void KHZ_ClientVisibilityRegisterCheckpoint();
bool KHZ_ClientPoseCanonical(khz::canonical::Hash& hash);
bool KHZ_ClientVisibilityCanonical(khz::canonical::Hash& hash);

namespace {
using namespace khz::checkpoint;
struct NativeClientImage {
    cg_s game;
    cgs_t gameStatic;
    centity_s entities[1024];
    weaponInfo_s weapons[128];
    float origins[1024][3];
    clientActive_t client;
    clientConnection_t connection;
    clientUIActive_t ui;
    AimAssistGlobals aim;
    int realtime, frametime, realFrametime;
    float mapCenter[3];
    const cg_s* gameAddress;
    const clientActive_t* clientAddress;
    const clientConnection_t* connectionAddress;
    const AimAssistGlobals* aimAddress;
    unsigned bodyCount;
    unsigned fpX87, fpMxcsr;
    bool captured;
};
NativeClientImage nativeImage; // raw BSS; never a multi-megabyte stack temporary
KHZ_ClientCheckpointProvider providers[DomainCount];
unsigned providerMask;
Frame saved;
std::atomic<bool> pinned{false}, requestPending{false};
unsigned capturedMask, restoredMask, validatedMask;
Failure failure;
char reason[256];
std::uint64_t captures, restorations, validations, failures;
bool requestSucceeded;
Operation requestedOperation;
std::uint64_t requestedFixture;
khz::canonical::Digest savedCanonical[khz::capture::DomainCount];
unsigned roundtripDomainMask;

bool canonicalDomains(khz::canonical::Digest* output, const Frame& frame, char* text, unsigned capacity) {
    const auto reject = [&](const char* value) { if (text && capacity) std::snprintf(text, capacity, "%s", value); return false; };
    if (!output || !KHZ_ClientOwnerCheckpointContext()) return reject("checkpoint_canonical_requires_parked_worker");
    const auto& game = KHZ_CGameArray()[0]; const auto& client = KHZ_ClientActiveArray()[0];
    if (!game.snap || !game.nextSnap) return reject("checkpoint_canonical_snapshot_pair_missing");
    khz::canonical::Hash player; KHZ_CanonicalPlayer(player, game.predictedPlayerState);
    output[unsigned(khz::capture::Domain::Player)] = player.finish();
    khz::canonical::Hash input; KHZ_CanonicalClientInput(input, client); KHZ_CanonicalClientFeedback(input, game);
    usercmd_s command{};
    if (!CL_GetUserCmd(0, client.cmdNumber, &command) || !KHZ_ClientInputCanonical(input))
        return reject("checkpoint_canonical_native_input_or_command_missing");
    KHZ_CanonicalCommand(input, command); output[unsigned(khz::capture::Domain::InputFeedback)] = input.finish();
    khz::WireHeader current{}, next{};
    if (!KHZ_CommittedSnapshotHeader(frame.currentSnapshotMessage, &current) ||
        !KHZ_CommittedSnapshotHeader(frame.nextSnapshotMessage, &next) || current.session != frame.session || next.session != frame.session)
        return reject("checkpoint_canonical_committed_snapshot_headers_missing");
    khz::canonical::Hash world;
    world.u64(current.tick); world.u64(current.input); world.u64(current.ack);
    world.u64(next.tick); world.u64(next.input); world.u64(next.ack);
    for (const auto* snapshot : {game.snap, game.nextSnap}) {
        if (snapshot->numEntities < 0 || snapshot->numEntities > 512 || snapshot->numClients < 0 || snapshot->numClients > 64)
            return reject("checkpoint_canonical_accepted_world_count_invalid");
        world.scalar(snapshot->snapFlags); world.scalar(snapshot->serverTime); world.scalar(snapshot->serverCommandSequence);
        KHZ_CanonicalPlayer(world, snapshot->ps); world.scalar(snapshot->numEntities); world.scalar(snapshot->numClients);
        for (int i = 0; i < snapshot->numEntities; ++i) KHZ_CanonicalEntity(world, snapshot->entities[i]);
        for (int i = 0; i < snapshot->numClients; ++i) KHZ_CanonicalClient(world, snapshot->clients[i]);
    }
    output[unsigned(khz::capture::Domain::AcceptedWorld)] = world.finish();
    khz::canonical::Hash pose; if (!KHZ_ClientPoseCanonical(pose)) return reject("checkpoint_canonical_native_pose_invalid");
    output[unsigned(khz::capture::Domain::ControllerPoseAnimation)] = pose.finish();
    khz::canonical::Hash aim; KHZ_CanonicalAim(aim, *AimAssist_GetGlobals(0));
    AimTarget* targets = nullptr; int count = 0; AimTarget_GetClientTargetList(0, &targets, &count);
    if (count < 0 || count > 64 || (count && !targets)) return reject("checkpoint_canonical_native_aim_list_invalid");
    aim.scalar(count);
    for (int i = 0; i < count; ++i) {
        aim.scalar(targets[i].entIndex); aim.scalar(targets[i].worldDistSqr); aim.array(targets[i].mins);
        aim.array(targets[i].maxs); aim.array(targets[i].velocity);
    }
    output[unsigned(khz::capture::Domain::Aim)] = aim.finish();
    khz::canonical::Hash effects;
    if (!KHZ_ClientVisibilityCanonical(effects)) return reject("checkpoint_canonical_native_effect_pool_invalid");
    output[unsigned(khz::capture::Domain::GameplayFX)] = effects.finish();
    khz::canonical::Hash rng; const auto random = KHZ_GetGameplayRandomState(); rng.scalar(random.crt); rng.scalar(random.hold);
    output[unsigned(khz::capture::Domain::RNG)] = rng.finish();
    for (unsigned domain = 0; domain < khz::capture::DomainCount; ++domain)
        if (!output[domain].valid) return reject("checkpoint_canonical_native_field_nonfinite");
    return true;
}

bool reject(char* output, unsigned capacity, const char* text) {
    if (output && capacity) std::snprintf(output, capacity, "%s", text);
    return false;
}
bool context(char* output, unsigned capacity) {
    return KHZ_IsClientSimulationThread() && KHZ_ClientOwnerCheckpointContext() ? true :
        reject(output, capacity, "checkpoint_requires_acknowledged_parked_worker");
}
bool snapshotPointer(const cg_s& game, const snapshot_s* snapshot) {
    return snapshot == &game.activeSnapshots[0] || snapshot == &game.activeSnapshots[1];
}
bool addressIn(const void* value, const void* base, std::size_t size) {
    const auto pointer = reinterpret_cast<std::uintptr_t>(value);
    const auto begin = reinterpret_cast<std::uintptr_t>(base);
    return pointer >= begin && pointer - begin < size;
}
bool sameAddress(const netadr_t& a, const netadr_t& b) {
    return a.type == b.type && a.port == b.port &&
        std::memcmp(a.ip, b.ip, sizeof(a.ip)) == 0 &&
        std::memcmp(a.ipx, b.ipx, sizeof(a.ipx)) == 0;
}
bool validNative(const cg_s& game, const clientActive_t& client,
    const clientConnection_t& connection, const clientUIActive_t& ui,
    char* output, unsigned capacity) {
    if (ui.connectionState != CA_ACTIVE || !ui.cgameInitialized || game.isLoading ||
        !snapshotPointer(game, game.snap) || !snapshotPointer(game, game.nextSnap))
        return reject(output, capacity, "native_checkpoint_requires_active_owned_snapshot_pair");
    if (connection.demorecording || connection.demoplaying || connection.isTimeDemo ||
        connection.demofile || connection.timeDemoLog)
        return reject(output, capacity, "native_checkpoint_does_not_restore_demo_file_handles");
    if (client.skelMemPos > sizeof(client.skelMemory) ||
        !addressIn(client.skelMemoryStart, client.skelMemory, sizeof(client.skelMemory)))
        return reject(output, capacity, "native_checkpoint_skeleton_arena_pointer_invalid");
    const auto& channel = connection.netchan;
    if (channel.fragmentBuffer != reinterpret_cast<const std::uint8_t*>(connection.netchanIncomingBuffer) ||
        channel.unsentBuffer != reinterpret_cast<const std::uint8_t*>(connection.netchanOutgoingBuffer) ||
        channel.fragmentBufferSize != sizeof(connection.netchanIncomingBuffer) ||
        channel.unsentBufferSize != sizeof(connection.netchanOutgoingBuffer) ||
        channel.fragmentLength || channel.unsentFragments || channel.unsentLength < 0 ||
        channel.unsentLength > channel.unsentBufferSize)
        return reject(output, capacity, "native_checkpoint_requires_drained_native_netchan");
    khz::canonical::Hash hash;
    KHZ_CanonicalPlayer(hash, game.predictedPlayerState);
    KHZ_CanonicalClientInput(hash, client); KHZ_CanonicalClientFeedback(hash, game);
    if (!hash.finish().valid) return reject(output, capacity, "native_checkpoint_has_nonfinite_gameplay_fields");
    if (!khz::isGameplayFpEnvironment(khz::gameplayFpEnvironment()))
        return reject(output, capacity, "native_checkpoint_floating_point_environment_changed");
    return true;
}
bool nativeCapture(const Frame&, char* output, unsigned capacity) {
    if (!context(output, capacity)) return false;
    const auto& game = KHZ_CGameArray()[0]; const auto& client = KHZ_ClientActiveArray()[0];
    const auto& connection = KHZ_ClientConnectionArray()[0]; const auto& ui = KHZ_ClientUIArray()[0];
    if (!validNative(game, client, connection, ui, output, capacity)) return false;
    const auto& state = KHZ_ClientStaticState();
    if (state.download || state.wwwDlInProgress || state.wwwDlDisconnected || state.downloadRestart)
        return reject(output, capacity, "native_checkpoint_does_not_restore_download_file_handles");
    std::memcpy(&nativeImage.game, &game, sizeof(game));
    std::memcpy(&nativeImage.gameStatic, &KHZ_CGameStaticArray()[0], sizeof(nativeImage.gameStatic));
    std::memcpy(nativeImage.entities, KHZ_CEntityArray()[0], sizeof(nativeImage.entities));
    std::memcpy(nativeImage.weapons, KHZ_CWeaponArray()[0], sizeof(nativeImage.weapons));
    std::memcpy(nativeImage.origins, KHZ_CEntityOriginArray()[0], sizeof(nativeImage.origins));
    std::memcpy(&nativeImage.client, &client, sizeof(client));
    std::memcpy(&nativeImage.connection, &connection, sizeof(connection));
    std::memcpy(&nativeImage.ui, &ui, sizeof(ui));
    auto* aim = AimAssist_GetGlobals(0);
    if (!aim) return reject(output, capacity, "native_checkpoint_private_aim_context_missing");
    std::memcpy(&nativeImage.aim, aim, sizeof(nativeImage.aim));
    nativeImage.realtime = state.realtime; nativeImage.frametime = state.frametime;
    nativeImage.realFrametime = state.realFrametime;
    std::memcpy(nativeImage.mapCenter, state.mapCenter, sizeof(nativeImage.mapCenter));
    nativeImage.gameAddress = &game; nativeImage.clientAddress = &client;
    nativeImage.connectionAddress = &connection; nativeImage.aimAddress = aim;
    nativeImage.bodyCount = KHZ_ClientBodyCount();
    const auto fp = khz::gameplayFpEnvironment(); nativeImage.fpX87 = fp.x87Control; nativeImage.fpMxcsr = fp.mxcsr;
    nativeImage.captured = true; return true;
}
bool nativeValidate(const Frame&, char* output, unsigned capacity) {
    if (!context(output, capacity)) return false;
    if (!nativeImage.captured) return reject(output, capacity, "native_checkpoint_image_missing");
    const auto& game = KHZ_CGameArray()[0]; const auto& client = KHZ_ClientActiveArray()[0];
    const auto& connection = KHZ_ClientConnectionArray()[0]; const auto& ui = KHZ_ClientUIArray()[0];
    if (!validNative(game, client, connection, ui, output, capacity)) return false;
    if (&game != nativeImage.gameAddress || &client != nativeImage.clientAddress ||
        &connection != nativeImage.connectionAddress || AimAssist_GetGlobals(0) != nativeImage.aimAddress ||
        KHZ_ClientBodyCount() != nativeImage.bodyCount || client.serverId != nativeImage.client.serverId ||
        !sameAddress(connection.serverAddress, nativeImage.connection.serverAddress) ||
        std::memcmp(client.mapname, nativeImage.client.mapname, sizeof(client.mapname)) != 0)
        return reject(output, capacity, "native_checkpoint_private_storage_or_asset_lifetime_changed");
    const auto fp = khz::gameplayFpEnvironment();
    if ((fp.x87Control & 0x0f3f) != (nativeImage.fpX87 & 0x0f3f) ||
        (fp.mxcsr & 0xffc0) != (nativeImage.fpMxcsr & 0xffc0))
        return reject(output, capacity, "native_checkpoint_floating_point_controls_changed");
    return true;
}
bool nativeRestore(const Frame& frame, std::uint64_t, char* output, unsigned capacity) {
    if (!nativeValidate(frame, output, capacity)) return false;
    // Pointer fields refer to the original same-process arenas and pinned map
    // assets, never to this saved image. Copying back to those exact addresses
    // preserves snapshot, skeleton, DObj/controller and netchan references.
    std::memcpy(&KHZ_CGameArray()[0], &nativeImage.game, sizeof(nativeImage.game));
    std::memcpy(&KHZ_CGameStaticArray()[0], &nativeImage.gameStatic, sizeof(nativeImage.gameStatic));
    std::memcpy(KHZ_CEntityArray()[0], nativeImage.entities, sizeof(nativeImage.entities));
    std::memcpy(KHZ_CWeaponArray()[0], nativeImage.weapons, sizeof(nativeImage.weapons));
    std::memcpy(KHZ_CEntityOriginArray()[0], nativeImage.origins, sizeof(nativeImage.origins));
    std::memcpy(&KHZ_ClientActiveArray()[0], &nativeImage.client, sizeof(nativeImage.client));
    std::memcpy(&KHZ_ClientConnectionArray()[0], &nativeImage.connection, sizeof(nativeImage.connection));
    std::memcpy(&KHZ_ClientUIArray()[0], &nativeImage.ui, sizeof(nativeImage.ui));
    std::memcpy(AimAssist_GetGlobals(0), &nativeImage.aim, sizeof(nativeImage.aim));
    auto& state = KHZ_ClientStaticState(); state.realtime = nativeImage.realtime;
    state.frametime = nativeImage.frametime; state.realFrametime = nativeImage.realFrametime;
    std::memcpy(state.mapCenter, nativeImage.mapCenter, sizeof(nativeImage.mapCenter));
    return true;
}
void nativeDiscard() { nativeImage.captured = false; }
void setFailure(Failure value, const char* text) {
    failure = value; ++failures;
    if (text != reason) std::snprintf(reason, sizeof(reason), "%s", text && *text ? text : "checkpoint_operation_rejected");
}
void discardCaptured() {
    for (unsigned reverse = DomainCount; reverse; ) {
        const auto domain = --reverse;
        if (capturedMask & (1u << domain)) providers[domain].discard();
    }
    capturedMask = restoredMask = validatedMask = roundtripDomainMask = 0;
    pinned.store(false, std::memory_order_release); saved = {};
}
bool workerRequest(void*) {
    requestSucceeded = false;
    if (!KHZ_ClientOwnerCheckpointContext() || !KHZ_IsClientSimulationThread()) {
        setFailure(Failure::NotParkedWorker, "checkpoint_requires_acknowledged_parked_worker"); return false;
    }
    if (requestedOperation == Operation::Discard) {
        discardCaptured(); failure = Failure::None; reason[0] = 0;
        requestSucceeded = true; return true;
    }
    Frame current; current.fixture = requestedFixture;
    if (!KHZ_ClientOwnerCheckpointObserve(&current, reason, sizeof(reason))) {
        setFailure(Failure::NotParkedWorker, reason); return false;
    }
    if (!valid(current)) { setFailure(Failure::InvalidIdentity, "checkpoint_owner_identity_or_clock_invalid"); return false; }
    if (providerMask != AllDomains) { setFailure(Failure::MissingProvider, "checkpoint_missing_actual_native_domain_provider"); return false; }
    if (requestedOperation == Operation::Capture) {
        if (pinned.load()) { setFailure(Failure::Busy, "discard_existing_checkpoint_before_replacing_it"); return false; }
        capturedMask = restoredMask = validatedMask = roundtripDomainMask = 0;
        // Pin lifetime BEFORE the first provider adds native references. A
        // callback failure cannot expose half-captured resources to map unload.
        pinned.store(true, std::memory_order_release); saved = current;
        for (unsigned domain = 0; domain < DomainCount; ++domain) {
            // A rejecting provider may have already pinned a prefix of native
            // references; its discard must also tolerate partial capture.
            capturedMask |= 1u << domain;
            if (!providers[domain].capture(saved, reason, sizeof(reason))) {
                setFailure(Failure::ProviderCapture, reason); discardCaptured(); return false;
            }
        }
        if (!canonicalDomains(savedCanonical, saved, reason, sizeof(reason))) {
            setFailure(Failure::ProviderCapture, reason); discardCaptured(); return false;
        }
        ++captures;
    } else {
        if (!pinned.load() || capturedMask != AllDomains) { setFailure(Failure::NoCheckpoint, "complete_private_checkpoint_not_captured"); return false; }
        if (!sameLifetime(saved, current)) { setFailure(Failure::InvalidIdentity, "checkpoint_session_generation_or_clock_changed"); return false; }
        validatedMask = restoredMask = roundtripDomainMask = 0;
        for (unsigned domain = 0; domain < DomainCount; ++domain) {
            if (!providers[domain].validateRestore(saved, reason, sizeof(reason))) {
                setFailure(Failure::ProviderValidation, reason); return false;
            }
            validatedMask |= 1u << domain;
        }
        ++validations;
        if (requestedOperation == Operation::Restore) {
            const auto epoch = khz::counter();
            for (unsigned domain = 0; domain < DomainCount; ++domain) {
                if (!providers[domain].restore(saved, epoch, reason, sizeof(reason))) {
                    // A partial restore remains pinned and parked. Never
                    // resume gameplay or advertise replay eligibility.
                    setFailure(Failure::ProviderRestore, reason); return false;
                }
                restoredMask |= 1u << domain;
            }
            Frame restored; restored.fixture = saved.fixture;
            khz::canonical::Digest observed[khz::capture::DomainCount];
            if (!KHZ_ClientOwnerCheckpointObserve(&restored, reason, sizeof(reason)) ||
                restored.logicalTick != saved.logicalTick || restored.currentSnapshotMessage != saved.currentSnapshotMessage ||
                restored.nextSnapshotMessage != saved.nextSnapshotMessage || !canonicalDomains(observed, restored, reason, sizeof(reason))) {
                setFailure(Failure::ProviderRestore, reason); return false;
            }
            for (unsigned domain = 0; domain < khz::capture::DomainCount; ++domain) {
                if (!khz::canonical::equal(savedCanonical[domain], observed[domain])) {
                    std::snprintf(reason, sizeof(reason), "native_private_restore_canonical_domain_%u_mismatch", domain);
                    setFailure(Failure::ProviderRestore, reason); return false;
                }
                roundtripDomainMask |= 1u << domain;
            }
            ++restorations;
        }
    }
    failure = Failure::None; reason[0] = 0; requestSucceeded = true; return true;
}
bool request(Operation operation, std::uint64_t fixture, unsigned timeout) {
    iassert(Sys_IsMainThread());
    bool expected = false;
    if (!requestPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    requestedOperation = operation; requestedFixture = fixture;
    if (!KHZ_ClientOwnerRunParkedTask(workerRequest, nullptr, timeout)) {
        // RunParkedTask retains request memory/map lifetime on timeout. Root
        // must stop/join before reuse; do not clear the in-flight request here.
        if (!KHZ_ClientOwnerParkedTaskOutstanding()) {
            setFailure(Failure::NotParkedWorker, "checkpoint_owner_rejected_parked_request");
            requestPending.store(false, std::memory_order_release);
        }
        return false;
    }
    requestPending.store(false, std::memory_order_release);
    return requestSucceeded;
}
}

void KHZ_InitClientCheckpoint() {
    iassert(Sys_IsMainThread());
    static bool initialized = false;
    if (initialized) return;
    const KHZ_ClientCheckpointProvider provider{Domain::NativeClient, sizeof(nativeImage),
        nativeCapture, nativeValidate, nativeRestore, nativeDiscard};
    KHZ_ClientCheckpointRegister(provider);
    KHZ_ClientOwnerRegisterCheckpoint(); KHZ_ClientPoseRegisterCheckpoint();
    KHZ_XAnimOwnerRegisterCheckpoint(); KHZ_ClientAimTargetsRegisterCheckpoint();
    KHZ_ClientProjectileRegisterCheckpoint(); KHZ_ClientVisibilityRegisterCheckpoint();
    initialized = true;
}
bool KHZ_ClientCheckpointRegister(const KHZ_ClientCheckpointProvider& provider) {
    iassert(Sys_IsMainThread());
    const auto domain = static_cast<unsigned>(provider.domain);
    if (KHZ_ClientOwnerRunning() || pinned.load(std::memory_order_acquire) || requestPending.load() ||
        domain >= DomainCount || (providerMask & (1u << domain)) || !provider.storageBytes ||
        !provider.capture || !provider.validateRestore || !provider.restore || !provider.discard) return false;
    providers[domain] = provider; providerMask |= 1u << domain; return true;
}
bool KHZ_ClientCheckpointCapture(std::uint64_t fixture, unsigned timeout) { return request(Operation::Capture, fixture, timeout); }
bool KHZ_ClientCheckpointValidateRestore(unsigned timeout) { return !requestPending.load() && request(Operation::ValidateRestore, saved.fixture, timeout); }
bool KHZ_ClientCheckpointRestore(unsigned timeout) { return !requestPending.load() && request(Operation::Restore, saved.fixture, timeout); }
bool KHZ_ClientCheckpointDiscard(unsigned timeout) { return !requestPending.load() && request(Operation::Discard, saved.fixture, timeout); }
bool KHZ_ClientCheckpointPinned() { return pinned.load(std::memory_order_acquire); }
bool KHZ_ClientCheckpointCanReleaseAssets() { return !pinned.load(std::memory_order_acquire) && !requestPending.load(std::memory_order_acquire); }
void KHZ_ClientCheckpointWorkerExit() {
    iassert(KHZ_ClientOwnerCheckpointContext());
    if (pinned.load(std::memory_order_acquire)) discardCaptured();
    if (requestPending.load(std::memory_order_acquire)) setFailure(Failure::OwnerFault, "checkpoint_owner_stopped_before_request_completed");
    requestPending.store(false, std::memory_order_release);
}
const char* KHZ_ClientCheckpointStatusJSON() {
    iassert(Sys_IsMainThread()); static char output[2048];
    if (requestPending.load(std::memory_order_acquire)) {
        std::snprintf(output, sizeof(output), "{\"schema\":1,\"pending\":true,\"assets_pinned\":%s,\"complete\":false,\"scope\":\"private same-process checkpoint; pending worker result cannot attest replay\"}", pinned.load() ? "true" : "false");
        return output;
    }
    std::size_t bytes = 0; for (unsigned domain = 0; domain < DomainCount; ++domain)
        if (providerMask & (1u << domain)) bytes += providers[domain].storageBytes;
    char escaped[512]; unsigned length = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(reason); *p && length + 2 < sizeof(escaped); ++p) {
        if (*p == '"' || *p == '\\') escaped[length++] = '\\';
        escaped[length++] = *p < 32 ? ' ' : static_cast<char>(*p);
    }
    escaped[length] = 0;
    std::snprintf(output, sizeof(output),
        "{\"schema\":1,\"pending\":false,\"assets_pinned\":%s,\"fixture\":%llu,\"session\":%llu,\"generation\":%llu,\"logical_tick\":%llu,\"provider_mask\":%u,\"captured_mask\":%u,\"validated_mask\":%u,\"restored_mask\":%u,\"required_domains\":%u,\"roundtrip_domain_mask\":%u,\"required_canonical_domains\":%u,\"storage_bytes\":%llu,\"captures\":%llu,\"validations\":%llu,\"restorations\":%llu,\"failures\":%llu,\"failure\":%u,\"reason\":\"%s\",\"complete\":%s,\"private_restore_complete\":%s,\"scope\":\"actual private client mementos; full replay additionally requires authoritative consumption timeline and native server world/script/RNG restoration\"}",
        pinned.load() ? "true" : "false", saved.fixture, saved.session, saved.generation, saved.logicalTick,
        providerMask, capturedMask, validatedMask, restoredMask, AllDomains, roundtripDomainMask,
        khz::capture::AllDomains, static_cast<unsigned long long>(bytes),
        captures, validations, restorations, failures, static_cast<unsigned>(failure), escaped,
        pinned.load() && capturedMask == AllDomains && failure == Failure::None ? "true" : "false",
        restoredMask == AllDomains && roundtripDomainMask == khz::capture::AllDomains && failure == Failure::None ? "true" : "false");
    return output;
}
