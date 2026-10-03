#include "khz_client_owner.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <qcommon/cmd.h>
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <cgame_mp/cg_local_mp.h>
#include <bgame/bg_public.h>
#include <aim_assist/aim_assist.h>
#include <khz/khz_collision.h>
#include <khz/khz_runtime.h>
#include <khz/khz_client_input.h>
#include <khz/khz_client_input_checkpoint.h>
#include <khz/khz_client_pose.h>
#include <khz/khz_client_visibility.h>
#include <khz/khz_client_body.h>
#include <khz/khz_client_projectile.h>
#include <khz/khz_controls.h>
#include <park_wait.hpp>
#include <khz/khz_screenshot.h>
#include <khz/khz_client_capture.h>
#include <khz/khz_client_checkpoint.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_workercmds.h>
#include <EffectsCore/fx_system.h>
#include <core.hpp>
#include <publication.hpp>
#include <fp_environment.hpp>
#include <gameplay_rng.hpp>
#include <universal/com_math.h>

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <setjmp.h>

namespace {
enum class Phase : unsigned { Stopped, Starting, Running, Parking, Parked, Fault, Stopping };
enum class ParkedTaskPhase : unsigned { Idle, Queued, Running, Finished, Fault };
constexpr unsigned SnapshotCapacity = 512;
constexpr unsigned VisualEventCapacity = 4096;
constexpr unsigned ReliableCapacity = 128;

void copySnapshot(snapshot_s& destination, const snapshot_s& source) {
    // Only accepted, active records are copied. Unused entity/client tails are
    // neither published nor inspected; the producer does not clear 140 KiB on
    // every 1 ms snapshot.
    destination.snapFlags = source.snapFlags;
    destination.ping = source.ping;
    destination.serverTime = source.serverTime;
    destination.ps = source.ps;
    destination.numEntities = source.numEntities;
    destination.numClients = source.numClients;
    destination.serverCommandSequence = source.serverCommandSequence;
    std::memcpy(destination.entities, source.entities, sizeof(entityState_s) * source.numEntities);
    std::memcpy(destination.clients, source.clients, sizeof(clientState_s) * source.numClients);
}
bool validSnapshot(const snapshot_s& snapshot) {
    if (snapshot.numEntities < 0 || snapshot.numEntities > 512 || snapshot.numClients < 0 || snapshot.numClients > 64
        || static_cast<unsigned>(snapshot.ps.clientNum) >= 64) return false;
    bool seen[1024]{};
    for (int i = 0; i < snapshot.numEntities; ++i) {
        const unsigned number = snapshot.entities[i].number;
        if (number >= 1024 || seen[number]) return false;
        seen[number] = true;
    }
    bool clientsSeen[64]{};
    for (int i = 0; i < snapshot.numClients; ++i) {
        const unsigned number = snapshot.clients[i].clientIndex;
        if (number >= 64 || clientsSeen[number]) return false;
        clientsSeen[number] = true;
    }
    return true;
}

enum class EventKind : unsigned { Entity, Damage, AnimationSound, MenuNotify };
struct VisualEvent {
    EventKind kind;
    std::uint64_t session, generation, tick;
    int time, event, entityNumber;
    entityState_s state;
    float origin[3], angles[3];
    int damageYaw, damagePitch, damageCount;
    snd_alias_list_t* alias;
};
struct ReliableCommand { char text[1024]; };
struct ServerCommand { int number; char text[1024]; };
struct MainKey { int key, down; unsigned milliseconds; };
template<class T, unsigned Capacity> class FixedJournal {
public:
    explicit FixedJournal(T* storage) : values_(storage) {}
    bool push(const T& value) {
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) >= Capacity) return false;
        values_[write & (Capacity - 1)] = value;
        write_.store(write + 1, std::memory_order_release); return true;
    }
    bool pop(T& value) {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = values_[read & (Capacity - 1)];
        read_.store(read + 1, std::memory_order_release); return true;
    }
    const T* front() const {
        const auto read = read_.load(std::memory_order_relaxed);
        return read == write_.load(std::memory_order_acquire) ? nullptr : &values_[read & (Capacity - 1)];
    }
    unsigned size() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }
private:
    static_assert(Capacity > 1 && !(Capacity & (Capacity - 1)));
    T* values_;
    alignas(64) std::atomic<unsigned> write_{0};
    alignas(64) std::atomic<unsigned> read_{0};
};
VisualEvent visualEventStorage[VisualEventCapacity];
ReliableCommand reliableCommandStorage[ReliableCapacity];
ServerCommand serverCommandStorage[SnapshotCapacity];
MainKey mainKeyStorage[64];
struct AnimationFeedback {
    float legsYaw, torsoYaw, torsoPitch;
    int legsYawing, torsoYawing, torsoPitching, turnAnimEndTime;
    char turnAnimType;
    float lerpMoveDir, lerpLean, playerAngles[3];
    unsigned conditions[NUM_ANIM_CONDITIONS][2];
};
struct PredictionFrame {
    std::uint64_t session, generation, tick;
    int time, oldTime, commandNumber, serverId, connectionState, latestSnapshotNumber, latestSnapshotTime;
    int realtime, frametime;
    clSnapshot_t authoritative;
    playerState_s player;
    entityState_s playerEntity;
    usercmd_s latestCommand;
    float predictedError[3];
    int predictedErrorTime, stepViewStart;
    float stepViewChange, kickAVel[3], kickAngles[3], offsetAngles[3], vGunSpeed[3], vGunOffset[3];
    float selectedLocation[2], zoomSensitivity, adsSensitivity;
    unsigned weaponSelect;
    unsigned weaponLatestPrimaryIdx;
    int weaponSelectTime, equippedOffHand, cursorHintIcon, cursorHintTime, proneBlockedEndTime;
    int invalidCmdHintType, invalidCmdHintTime, ammoFadeTime, lastActionSlotTime;
    int landTime;
    float landChange;
    int positionToADS;
    float lastWeaponPosFrac;
    AnimationFeedback animation;
    int currentSnapshotNumber;
    snapshot_s currentWorld, nextWorld;
};
// Large POD domains use loader-zeroed BSS, never aggregate temporaries in the
// CRT's Owner constructor. Some Win32 client records exceed its 1 MiB stack.
cg_s ownerGameStorage[1];
cgs_t ownerGameStaticStorage[1];
centity_s ownerEntityStorage[1][1024];
weaponInfo_s ownerWeaponStorage[1][128];
float ownerOriginStorage[1][1024][3];
clientActive_t presentationClientStorage[1];
clientConnection_t presentationConnectionStorage[1];
clientUIActive_t presentationUIStorage[1];
clientStatic_t presentationStaticStorage;
snapshot_s incomingSnapshotStorage;
char ownerBigConfigString[8192];
alignas(64) PredictionFrame predictionFrameStorage[3];
const PredictionFrame* presentationFrame = nullptr; // main's pinned immutable record

struct Owner {
    std::atomic<Phase> phase{Phase::Stopped};
    std::atomic<bool> active{false}, stop{false}, park{false}, reasonClaimed{false};
    std::atomic<int> nativeConnection{CA_DISCONNECTED};
    std::atomic<bool> nativeCgameInitialized{false};
    std::atomic<bool> resumeControls{false};
    std::atomic<bool> resumeCheckpoint{false};
    std::atomic<bool> checkpointParkRequested{false};
    std::atomic<ParkedTaskPhase> parkedTaskPhase{ParkedTaskPhase::Idle};
    bool (*parkedTaskCallback)(void*) = nullptr;
    void* parkedTaskContext = nullptr;
    std::atomic<bool> fault{false};
    std::atomic<bool> assetRebuild{false};
    HANDLE thread = nullptr, wake = nullptr, ready = nullptr, parked = nullptr, parkedTaskDone = nullptr;
    KHZ_ClientOwnerHooks hooks{};
    std::uint64_t session = 0, generation = 0;
    khz::GameplayRandomState randomState{1u, 0u};
    khz::DeadlineClock clock;
    std::atomic<std::uint64_t> tick{0}, deadline{0};
    char reason[256]{};
    char faultReason[256]{};
    int errorCode = 0;
    cg_s (&game)[1] = ownerGameStorage;
    cgs_t (&gameStatic)[1] = ownerGameStaticStorage;
    centity_s (&entities)[1][1024] = ownerEntityStorage;
    weaponInfo_s (&weapons)[1][128] = ownerWeaponStorage;
    float (&origins)[1][1024][3] = ownerOriginStorage;
    clientActive_t (&presentationClient)[1] = presentationClientStorage;
    clientConnection_t (&presentationConnection)[1] = presentationConnectionStorage;
    clientUIActive_t (&presentationUI)[1] = presentationUIStorage;
    clientStatic_t& presentationStatic = presentationStaticStorage;
    snapshot_s& incoming = incomingSnapshotStorage;
    int processedSnapshot = 0;
    int currentSnapshotNumber = 0;
    std::atomic<int> publishedSnapshotNumber{0}, publishedSnapshotTime{0};
    FixedJournal<VisualEvent, VisualEventCapacity> events{visualEventStorage};
    FixedJournal<ReliableCommand, ReliableCapacity> reliable{reliableCommandStorage};
    FixedJournal<ServerCommand, SnapshotCapacity> serverCommands{serverCommandStorage};
    FixedJournal<MainKey, 64> mainKeys{mainKeyStorage};
    std::atomic<int> keyCatchers{0};
    bool locationSelection = false; // simulation-owner only; separate from frontend mask
    std::uint64_t inputCounterEpoch = 0;
    unsigned inputMillisecondsEpoch = 0;
    int copiedServerCommand = 0;
    int deliveredServerCommand = 0; // sole presentation consumer
    int startupTransportSnapshot = 0; // main-owned reader-domain handoff evidence
    std::atomic<int> publishedServerCommand{0};
    std::atomic<bool> mainServerCommand{false}, mapLifecycle{false};
    int pendingServerCommand = 0, fragmentIndex = -1, fragmentLast = 0;
    bool refreshIncoming = false, pendingAssetMetadata = false, pendingFinishesFragment = false;
    std::uint64_t mainServerBarriers = 0, assetRefreshes = 0;
    khz::ExternalTriplePublication<PredictionFrame> prediction{predictionFrameStorage};
    AimAssistGlobals aim{};
    bool privateDomainsReady = false, poseInitialized = false;
    std::uint64_t nextStartAttempt = 0, startAttempts = 0, startFailures = 0;
    char startStage[32] = "idle", startFailureStage[32]{}, startFailureReason[256]{};
    khz::CollisionMapToken map{};
    khz::CollisionFrame collision{};
    khz::CollisionBuildState collisionBuild{};
    std::atomic<std::uint64_t> ticks{0}, snapshotsAccepted{0}, eventsWritten{0}, eventsRead{0};
    std::atomic<std::uint64_t> queueFailures{0}, parks{0}, lateMaximum{0}, debtMaximum{0}, aimStaleTicks{0};
    std::atomic<std::uint64_t> presentationSnapshotsSkipped{0};
    std::atomic<int> presentationSnapshotNumber{0};
    std::atomic<std::uint64_t> poseTicks{0};
    std::atomic<unsigned> fpX87{0}, fpMxcsr{0};
    std::atomic<bool> fpConfigured{false};
    std::uint64_t consumedTick = 0;
    std::uint64_t completedTick = 0;
};
Owner owner;
thread_local bool simulationThread = false;
thread_local bool guardedCallback = false;
thread_local bool visualReplay = false;
thread_local bool controlAccess = false;
thread_local bool checkpointContext = false;
thread_local jmp_buf callbackJump;

struct OwnerCheckpointImage {
    bool captured, poseInitialized, locationSelection;
    int processedSnapshot, currentSnapshotNumber, publishedSnapshotNumber,
        publishedSnapshotTime, copiedServerCommand, publishedServerCommand, keyCatchers;
    std::uint64_t completedTick;
    khz::GameplayRandomState randomState;
    khz::CollisionMapToken map;
    khz::CollisionBuildState collisionBuild;
    snapshot_s incoming;
    PredictionFrame presentation;
};
OwnerCheckpointImage ownerCheckpoint; // raw BSS, never a snapshot stack temporary

void updateMaximum(std::atomic<std::uint64_t>& value, std::uint64_t sample) {
    auto observed = value.load(std::memory_order_relaxed);
    while (sample > observed && !value.compare_exchange_weak(observed, sample, std::memory_order_relaxed)) {}
}
void failQueue(const char* name) {
    ++owner.queueFailures;
    KHZ_ClientOwnerRequestPark(name);
}
bool failStart(const char* reason) {
    ++owner.startFailures;
    std::snprintf(owner.startFailureStage, sizeof(owner.startFailureStage), "%s", owner.startStage);
    std::snprintf(owner.startFailureReason, sizeof(owner.startFailureReason), "%s", reason);
    owner.nextStartAttempt = khz::counter() + khz::frequency() / 2;
    owner.phase.store(Phase::Stopped, std::memory_order_release);
    return false;
}
void jsonString(char* destination, std::size_t capacity, const char* source) {
    std::size_t length = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(source); *p; ++p) {
        const bool escape = *p == '"' || *p == '\\';
        if (length + (escape ? 2 : 1) >= capacity) break;
        if (escape) destination[length++] = '\\';
        destination[length++] = *p < 32 ? ' ' : static_cast<char>(*p);
    }
    destination[length] = 0;
}
void queueEvent(const VisualEvent& event) {
    if (KHZ_IsHeadless()) return;
    if (!owner.events.push(event)) failQueue("Client visual event journal exhausted; presentation must catch up before ownership resumes");
    else ++owner.eventsWritten;
}
VisualEvent eventHeader(EventKind kind) {
    VisualEvent event{};
    event.kind = kind;
    event.session = owner.session; event.generation = owner.generation;
    event.tick = owner.tick.load(std::memory_order_relaxed); event.time = owner.game[0].time;
    return event;
}
void copyAnimation(AnimationFeedback& output, const clientInfo_t& input) {
    output.legsYaw = input.legs.yawAngle; output.legsYawing = input.legs.yawing;
    output.torsoYaw = input.torso.yawAngle; output.torsoYawing = input.torso.yawing;
    output.torsoPitch = input.torso.pitchAngle; output.torsoPitching = input.torso.pitching;
    output.turnAnimType = input.turnAnimType; output.turnAnimEndTime = input.turnAnimEndTime;
    output.lerpMoveDir = input.lerpMoveDir; output.lerpLean = input.lerpLean;
    std::memcpy(output.playerAngles, input.playerAngles, sizeof(output.playerAngles));
    std::memcpy(output.conditions, input.clientConditions, sizeof(output.conditions));
}
void applyAnimation(clientInfo_t& output, const AnimationFeedback& input) {
    output.legs.yawAngle = input.legsYaw; output.legs.yawing = input.legsYawing;
    output.torso.yawAngle = input.torsoYaw; output.torso.yawing = input.torsoYawing;
    output.torso.pitchAngle = input.torsoPitch; output.torso.pitching = input.torsoPitching;
    output.turnAnimType = input.turnAnimType; output.turnAnimEndTime = input.turnAnimEndTime;
    output.lerpMoveDir = input.lerpMoveDir; output.lerpLean = input.lerpLean;
    std::memcpy(output.playerAngles, input.playerAngles, sizeof(input.playerAngles));
    std::memcpy(output.clientConditions, input.conditions, sizeof(input.conditions));
}
void respawnGameplay(cg_s& game, const playerState_s& state) {
    game.predictedPlayerState = state;
    game.weaponSelect = state.weapon; game.weaponSelectTime = game.time;
    game.equippedOffHand = state.offHandIndex;
    game.cursorHintIcon = HINT_NONE; game.cursorHintTime = 0; game.proneBlockedEndTime = 0;
    std::fill_n(game.kickAngles, 3, 0.f); std::fill_n(game.kickAVel, 3, 0.f);
    std::fill_n(game.offsetAngles, 3, 0.f); std::fill_n(game.vGunSpeed, 3, 0.f);
    std::fill_n(game.predictedError, 3, 0.f);
    game.adsViewErrorDone = 0; game.stepViewChange = 0.f; game.stepViewStart = game.time;
    CL_SetStance(0, CL_STANCE_STAND); CL_SetADS(0, 0);
}
void acceptGameplaySnapshot(int number, const snapshot_s& snapshot) {
    auto& game = owner.game[0];
    auto* old = game.nextSnap ? game.nextSnap : game.snap;
    const bool respawn = !old || old->ps.clientNum != snapshot.ps.clientNum ||
        old->ps.stats[STAT_SPAWN_COUNT] != snapshot.ps.stats[STAT_SPAWN_COUNT] ||
        ((old->snapFlags ^ snapshot.snapFlags) & 4) != 0;
    const bool teleported = old && ((old->ps.eFlags ^ snapshot.ps.eFlags) & 2) != 0;
    auto* destination = &game.activeSnapshots[old == &game.activeSnapshots[0] ? 1 : 0];
    copySnapshot(*destination, snapshot);
    game.snap = old ? old : destination; game.nextSnap = destination;
    game.latestSnapshotNum = number; game.latestSnapshotTime = snapshot.serverTime;
    game.bgs.latestSnapshotTime = snapshot.serverTime;
    game.playerTeleported = teleported || respawn;
    game.identifyClientNum = snapshot.ps.stats[STAT_IDENT_CLIENT_NUM];
    for (auto& info : game.bgs.clientinfo) { info.nextValid = false; info.infoValid = false; }
    for (int i = 0; i < snapshot.numClients; ++i) {
        const auto& state = snapshot.clients[i];
        auto& info = game.bgs.clientinfo[state.clientIndex];
        info.oldteam = info.team; info.team = state.team;
        info.infoValid = info.nextValid = true; info.clientNum = state.clientIndex;
        info.rank = state.rank; info.prestige = state.prestige; info.perks = state.perks;
        info.attachedVehEntNum = state.attachedVehEntNum; info.attachedVehSlotIndex = state.attachedVehSlotIndex;
        std::memcpy(info.name, state.name, sizeof(info.name));
    }
    // Event feedback must see the accepted spawn, team and perk state, rather
    // than the previous snapshot's metadata or prediction after a respawn.
    if (respawn) respawnGameplay(game, snapshot.ps);
    bool present[1024]{};
    for (int i = 0; i < snapshot.numEntities; ++i) {
        const auto& state = snapshot.entities[i];
        auto& entity = owner.entities[0][state.number];
        const bool reset = !entity.nextValid || entity.nextState.eType != state.eType ||
            ((entity.nextState.lerp.eFlags ^ state.lerp.eFlags) & 2) != 0;
        if (entity.nextValid) entity.currentState = entity.nextState.lerp;
        else entity.currentState = state.lerp;
        entity.nextState = state; entity.nextValid = true; present[state.number] = true;
        if (reset) { entity.previousEventSequence = 0; entity.miscTime = 0; }
        // These private poses serve item/event arithmetic only. They are never
        // sampled for movement collision or sent to the visual DObj system.
        BG_EvaluateTrajectory(&state.lerp.pos, game.time, entity.pose.origin);
        BG_EvaluateTrajectory(&state.lerp.apos, game.time, entity.pose.angles);
    }
    if ((snapshot.ps.otherFlags & 6) && static_cast<unsigned>(snapshot.ps.clientNum) < 64) {
        const auto number = snapshot.ps.clientNum;
        auto& local = owner.entities[0][number];
        if (local.nextValid) local.currentState = local.nextState.lerp;
        auto localState = snapshot.ps;
        BG_PlayerStateToEntityState(&localState, &local.nextState, 0, 0);
        local.nextState.number = static_cast<unsigned short>(number);
        if (!local.nextValid || respawn || teleported) local.currentState = local.nextState.lerp;
        local.nextValid = true; local.pose.localClientNum = 0; present[number] = true;
    }
    for (unsigned i = 0; i < 1024; ++i) if (!present[i]) owner.entities[0][i].nextValid = false;
    if (teleported) std::fill_n(game.predictedError, 3, 0.f);
    CG_SetFrameInterpolation(0);
    if (!KHZ_ClientPoseAccept(game.nextSnap)) {
        KHZ_ClientOwnerRequestAssetRebuild("Accepted player model requires parked gameplay pose preparation"); return;
    }
    for (int i = 0; i < snapshot.numEntities; ++i) CG_CheckEvents(0, &owner.entities[0][snapshot.entities[i].number]);
    CG_BuildItemList(0, game.nextSnap);
    owner.currentSnapshotNumber = owner.publishedSnapshotNumber.load(std::memory_order_relaxed);
    owner.publishedSnapshotTime.store(snapshot.serverTime, std::memory_order_relaxed);
    owner.publishedSnapshotNumber.store(number, std::memory_order_release);
    ++owner.snapshotsAccepted;
}

bool mainConfigString(int index) {
    return index == CS_SERVERINFO || index == CS_SYSTEMINFO ||
        (index >= CS_CODINFO && index <= CS_CODINFO_VALUE_LAST) ||
        (index >= CS_MODELS && index <= CS_MODELS_LAST) ||
        (index >= CS_EFFECT_NAMES && index <= CS_SHELLSHOCKS_LAST) ||
        (index >= CS_TAGS && index <= CS_ITEMS);
}
int reliableConfigIndex() {
    char* end = nullptr;
    const auto value = std::strtol(Cmd_Argv(1), &end, 10);
    if (!Cmd_Argv(1)[0] || !end || *end || value < 0 || value >= MAX_CONFIGSTRINGS)
        Com_Error(ERR_DROP, "Invalid reliable configstring index");
    return static_cast<int>(value);
}
void appendConfigFragment(const char* text) {
    const auto used = std::strlen(ownerBigConfigString), added = std::strlen(text);
    if (used + added >= sizeof(ownerBigConfigString)) Com_Error(ERR_DROP, "Owner fragmented configstring exceeded BIG_INFO_STRING");
    std::memcpy(ownerBigConfigString + used, text, added + 1);
}
void publishServerCommands(int through) {
    const auto& connection = khz_clientConnections[0];
    if (through > connection.serverCommandSequence || through - owner.copiedServerCommand > 128) {
        Com_Error(ERR_DROP, "Owner received server commands outside the accepted reliable window"); return;
    }
    while (owner.copiedServerCommand < through && !owner.park.load()) {
        ServerCommand command{};
        command.number = owner.copiedServerCommand + 1;
        if (command.number <= owner.publishedServerCommand.load(std::memory_order_relaxed))
            Com_Error(ERR_DROP, "Reliable command published twice: next=%d copied=%d published=%d main_barrier=%d pending=%d generation=%llu",
                command.number, owner.copiedServerCommand, owner.publishedServerCommand.load(),
                int(owner.mainServerCommand.load()), owner.pendingServerCommand, owner.generation);
        const auto* text = connection.serverCommands[command.number & 127];
        std::snprintf(command.text, sizeof(command.text), "%s", text);
        if (command.number <= connection.serverCommandSequence - MAX_RELIABLE_COMMANDS)
            Com_Error(ERR_DROP, "Owner reliable server command cycled out before acceptance");
        // Service messages enqueue bounded metadata from the packet owner.
        // The main replay hook consumes the same prefix without recapturing.
        KHZ_ScreenshotClientServerCommand(command.text);
        // A lifecycle cursor remains uncommitted. Stop/native snapshot catch-up
        // executes the command exactly once from the still-pinned CL ring.
        if (text[0] == 'B' || text[0] == 'n' || text[0] == 'w' || text[0] == 'v') {
            if (text[0] != 'v') {
                owner.mapLifecycle.store(true, std::memory_order_release);
                KHZ_ClientOwnerRequestPark("Reliable server command requires native map/disconnect lifetime work"); return;
            }
        }
        Cmd_TokenizeStringWithLimit(command.text, 3);
        const char kind = Cmd_Argv(0)[0];
        int index = -1;
        if (kind == 'd' || kind == 'x') index = reliableConfigIndex();
        if (kind == 'y' || kind == 'z') {
            index = reliableConfigIndex();
            if (owner.fragmentIndex != index || owner.fragmentLast + 1 != command.number)
                Com_Error(ERR_DROP, "Owner reliable configstring fragments are missing or reordered");
            if (std::strlen(ownerBigConfigString) + std::strlen(Cmd_Argv(2)) >= sizeof(ownerBigConfigString))
                Com_Error(ERR_DROP, "Owner reliable configstring fragment exceeded BIG_INFO_STRING");
        }
        if (!owner.serverCommands.push(command)) {
            Cmd_EndTokenizedString();
            // Drain the copied prefix under a main barrier; never consume or
            // lose this unqueued command, and never replay its button effects.
            owner.pendingServerCommand = owner.copiedServerCommand;
            owner.mainServerCommand.store(true, std::memory_order_release);
            KHZ_ClientOwnerRequestPark("Reliable presentation journal requires ordered prefix drain"); return;
        }
        owner.publishedServerCommand.store(command.number, std::memory_order_release);
        if (kind == 'v' || ((kind == 'd' || kind == 'z') && mainConfigString(index))) {
            Cmd_EndTokenizedString();
            owner.pendingServerCommand = command.number;
            owner.pendingAssetMetadata = kind != 'v' && index >= CS_MODELS;
            owner.pendingFinishesFragment = kind == 'z';
            owner.mainServerCommand.store(true, std::memory_order_release);
            KHZ_ClientOwnerRequestPark("Reliable config/dvar command requires native main metadata work"); return;
        }
        if (kind == 'd') {
            CL_ConfigstringModified(0);
            if (index == CS_MAPCENTER) CL_ParseMapCenter(0);
        } else if (kind == 'x') {
            const auto length = std::snprintf(ownerBigConfigString, sizeof(ownerBigConfigString), "d %s %s", Cmd_Argv(1), Cmd_Argv(2));
            if (length < 0 || static_cast<unsigned>(length) >= sizeof(ownerBigConfigString)) Com_Error(ERR_DROP, "Owner initial configstring fragment is too long");
            owner.fragmentIndex = index; owner.fragmentLast = command.number;
        } else if (kind == 'y' || kind == 'z') {
            appendConfigFragment(Cmd_Argv(2)); owner.fragmentLast = command.number;
            if (kind == 'z') {
                Cmd_EndTokenizedString(); Cmd_TokenizeStringWithLimit(ownerBigConfigString, 3);
                CL_ConfigstringModified(0);
                if (index == CS_MAPCENTER) CL_ParseMapCenter(0);
                owner.fragmentIndex = -1; ownerBigConfigString[0] = 0;
            }
        } else if (kind == 'C') {
            const auto weapon = std::strtoul(Cmd_Argv(1), nullptr, 10);
            if (!weapon || (BG_ValidateWeaponNumber(weapon) && BG_GetWeaponDef(weapon)->offhandClass)) owner.game[0].equippedOffHand = weapon;
        } else if (kind == 'a') {
            const auto weapon = std::strtoul(Cmd_Argv(1), nullptr, 10);
            if (!weapon || BG_ValidateWeaponNumber(weapon)) CG_SelectWeaponIndex(0, weapon);
        }
        Cmd_EndTokenizedString();
        owner.copiedServerCommand = command.number;
        owner.gameStatic[0].serverCommandSequence = command.number;
        khz_clientConnections[0].lastExecutedServerCommand = command.number;
    }
}

DWORD WINAPI threadMain(void*) {
    simulationThread = true;
    Sys_InitClientSimulationThread();
    const auto floatingPoint = khz::configureGameplayFpEnvironment();
    owner.fpX87.store(floatingPoint.x87Control, std::memory_order_relaxed);
    owner.fpMxcsr.store(floatingPoint.mxcsr, std::memory_order_relaxed);
    owner.fpConfigured.store(khz::isGameplayFpEnvironment(floatingPoint), std::memory_order_release);
    SetThreadName(GetCurrentThreadId(), "ClientSimulation1kHz");
    owner.clock.reset(khz::counter(), khz::frequency());
    owner.inputCounterEpoch = khz::counter();
    owner.inputMillisecondsEpoch = Sys_Milliseconds();
    KHZ_ClientInputOwnerInit(owner.generation);
    owner.phase.store(Phase::Running, std::memory_order_release);
    SetEvent(owner.ready);
    while (!owner.stop.load(std::memory_order_acquire)) {
        // A diagnostic replay barrier stops at an actually completed tick.
        // Ordinary lifecycle/error barriers can still interrupt immediately.
        if (owner.checkpointParkRequested.load(std::memory_order_acquire) && !owner.park.load() &&
            owner.completedTick == owner.tick.load(std::memory_order_relaxed))
            KHZ_ClientOwnerRequestPark("Complete gameplay tick checkpoint barrier");
        if (owner.park.load(std::memory_order_acquire)) {
            KHZ_UnbindPredictionCollisionFrame();
            bgs = nullptr;
            guardedCallback = false;
            if (owner.phase.load() != Phase::Fault) owner.phase.store(Phase::Parked, std::memory_order_release);
            ++owner.parks;
            SetEvent(owner.parked);
            if (owner.parkedTaskPhase.load(std::memory_order_acquire) == ParkedTaskPhase::Queued) {
                owner.parkedTaskPhase.store(ParkedTaskPhase::Running, std::memory_order_release);
                checkpointContext = true;
                if (setjmp(callbackJump) == 0) {
                    guardedCallback = true;
                    owner.parkedTaskCallback(owner.parkedTaskContext);
                    guardedCallback = false;
                    owner.parkedTaskPhase.store(ParkedTaskPhase::Finished, std::memory_order_release);
                } else {
                    guardedCallback = false;
                    owner.phase.store(Phase::Fault, std::memory_order_release);
                    owner.parkedTaskPhase.store(ParkedTaskPhase::Fault, std::memory_order_release);
                }
                checkpointContext = false;
                KHZ_UnbindPredictionCollisionFrame(); bgs = nullptr;
                SetEvent(owner.parkedTaskDone);
            }
            WaitForSingleObject(owner.wake, INFINITE);
            continue;
        }
        if (owner.resumeCheckpoint.exchange(false, std::memory_order_acq_rel)) {
            // Input TLS and private histories were restored on this same
            // worker. Do not call Init/Resume and discard their replay state.
            const auto epoch = khz::counter();
            const auto milliseconds = KHZ_ClientOwnerMillisecondsAt(owner.deadline.load());
            if (!owner.clock.rebase(epoch, owner.clock.frequency(), owner.tick.load()) ||
                (KHZ_ClientInputReplayActive() && !KHZ_ClientInputReplayRebaseEpoch(epoch))) {
                std::snprintf(owner.faultReason, sizeof(owner.faultReason), "Checkpoint resume could not rebase the actual private scheduler/input timeline");
                owner.errorCode = ERR_DROP; owner.fault.store(true, std::memory_order_release);
                owner.park.store(true, std::memory_order_release); owner.phase.store(Phase::Fault, std::memory_order_release);
                SetEvent(owner.ready); continue;
            }
            owner.inputCounterEpoch = epoch; owner.inputMillisecondsEpoch = milliseconds;
            owner.deadline.store(epoch);
            owner.phase.store(Phase::Running, std::memory_order_release);
            SetEvent(owner.ready);
        }
        if (owner.resumeControls.exchange(false, std::memory_order_acq_rel)) {
            KHZ_ClientInputOwnerResume(owner.generation);
            owner.phase.store(Phase::Running, std::memory_order_release);
            SetEvent(owner.ready);
        }
        const auto now = khz::counter();
        const unsigned due = owner.clock.due(now);
        if (!due) {
            khz::waitUntilOrEvent(owner.clock.deadline(), owner.wake);
            continue;
        }
        updateMaximum(owner.debtMaximum, owner.clock.debt(now));
        for (unsigned pass = 0; pass < due && !owner.park.load() && !owner.stop.load(); ++pass) {
            const auto deadline = owner.clock.deadline();
            const auto started = khz::counter();
            if (started > deadline) updateMaximum(owner.lateMaximum, (started - deadline) * 1000000 / khz::frequency());
            owner.clock.advance();
            owner.tick.store(owner.clock.tick(), std::memory_order_relaxed);
            owner.deadline.store(deadline, std::memory_order_relaxed);
            if (setjmp(callbackJump) != 0) {
                guardedCallback = false;
                owner.phase.store(Phase::Fault, std::memory_order_release);
                owner.park.store(true, std::memory_order_release);
                break;
            }
            guardedCallback = true;
            owner.hooks.pumpTransport();
            if (!owner.park.load(std::memory_order_acquire)) {
                KHZ_ClientInputOwnerPoll(deadline);
                if (!owner.park.load(std::memory_order_acquire)) owner.hooks.tick(owner.clock.tick(), deadline);
            }
            guardedCallback = false;
            ++owner.ticks;
        }
        // Retain all debt. A bounded pass gives lifecycle/input work a chance;
        // it never throws away gameplay ticks to catch up with presentation.
        if (owner.clock.debt(khz::counter())) SwitchToThread();
    }
    checkpointContext = true;
    KHZ_ClientCheckpointWorkerExit();
    checkpointContext = false;
    KHZ_ClientInputOwnerStop();
    KHZ_UnbindPredictionCollisionFrame(); bgs = nullptr;
    Sys_ReleaseClientSimulationThread();
    simulationThread = false;
    SetEvent(owner.parked);
    return 0;
}
void closeHandle(HANDLE& handle) { if (handle) CloseHandle(handle); handle = nullptr; }
const char* phaseName(Phase phase) {
    switch (phase) {
    case Phase::Stopped: return "stopped"; case Phase::Starting: return "starting";
    case Phase::Running: return "running"; case Phase::Parking: return "parking";
    case Phase::Parked: return "parked"; case Phase::Fault: return "fault";
    case Phase::Stopping: return "stopping";
    } return "invalid";
}
}

bool KHZ_IsClientSimulationThread() { return simulationThread; }
bool KHZ_ClientOwnerCheckpointContext() { return simulationThread && checkpointContext; }
bool KHZ_ClientOwnerParkedTaskOutstanding() { return owner.parkedTaskPhase.load(std::memory_order_acquire) != ParkedTaskPhase::Idle; }
bool KHZ_ClientOwnerParkCheckpoint(unsigned timeout) {
    iassert(Sys_IsMainThread());
    if (!owner.active.load() || owner.fault.load() || owner.stop.load() || controlAccess) return false;
    if (owner.phase.load() == Phase::Parked) return owner.completedTick == owner.tick.load();
    ResetEvent(owner.parked);
    owner.checkpointParkRequested.store(true, std::memory_order_release); SetEvent(owner.wake);
    return WaitForSingleObject(owner.parked, timeout) == WAIT_OBJECT_0 &&
        owner.phase.load() == Phase::Parked && owner.completedTick == owner.tick.load();
}
bool KHZ_ClientOwnerRunParkedTask(bool (*callback)(void*), void* context, unsigned timeout) {
    iassert(Sys_IsMainThread());
    if (!callback || controlAccess || !owner.active.load(std::memory_order_acquire) ||
        owner.phase.load(std::memory_order_acquire) != Phase::Parked || !owner.park.load() ||
        owner.stop.load() || owner.fault.load() || bgs || !owner.parkedTaskDone) return false;
    if (!KHZ_IsHeadless() && owner.presentationStatic.rendererStarted) { R_SyncRenderThread(); R_WaitWorkerCmds(); }
    if (owner.parkedTaskPhase.load(std::memory_order_acquire) != ParkedTaskPhase::Idle) return false;
    owner.parkedTaskCallback = callback; owner.parkedTaskContext = context;
    ResetEvent(owner.parkedTaskDone);
    owner.parkedTaskPhase.store(ParkedTaskPhase::Queued, std::memory_order_release);
    SetEvent(owner.wake);
    if (WaitForSingleObject(owner.parkedTaskDone, timeout) != WAIT_OBJECT_0) return false;
    const auto result = owner.parkedTaskPhase.load(std::memory_order_acquire);
    if (result != ParkedTaskPhase::Finished) return false;
    owner.parkedTaskCallback = nullptr; owner.parkedTaskContext = nullptr;
    owner.parkedTaskPhase.store(ParkedTaskPhase::Idle, std::memory_order_release);
    // Completion is independent of the callback's operation result. The
    // caller reads its own release-published result after this acknowledgement.
    return true;
}
bool KHZ_ClientOwnerCheckpointObserve(KHZ_ClientCheckpointFrame* frame, char* reason, unsigned capacity) {
    auto reject = [&](const char* text) { if (reason && capacity) std::snprintf(reason, capacity, "%s", text); return false; };
    if (!frame || !checkpointContext || !simulationThread || owner.phase.load() != Phase::Parked ||
        !owner.park.load() || !owner.active.load() || owner.fault.load() || bgs || KHZ_HasPredictionCollisionFrame())
        return reject("owner_checkpoint_requires_actual_parked_worker_boundary");
    if (!owner.privateDomainsReady || !owner.poseInitialized || owner.completedTick != owner.tick.load() ||
        !presentationFrame || presentationFrame->tick != owner.completedTick)
        return reject("owner_checkpoint_requires_complete_presented_gameplay_tick");
    if (owner.events.size() || owner.reliable.size() || owner.serverCommands.size() || owner.mainKeys.size() ||
        owner.mainServerCommand.load() || owner.mapLifecycle.load() || owner.assetRebuild.load() ||
        owner.pendingServerCommand || owner.refreshIncoming || owner.pendingAssetMetadata ||
        owner.pendingFinishesFragment || owner.fragmentIndex != -1)
        return reject("owner_checkpoint_requires_empty_ordered_journals_and_config_transactions");
    if (!KHZ_InputOwnerCheckpointParked()) return reject("owner_checkpoint_raw_producer_not_acknowledged_and_drained");
    const auto current = khz::captureCollisionMapToken(owner.session, owner.generation);
    if (current.models != owner.map.models || current.planes != owner.map.planes ||
        current.checksum != owner.map.checksum || current.session != owner.map.session || current.generation != owner.map.generation)
        return reject("owner_checkpoint_collision_map_lifetime_changed");
    frame->session = owner.session; frame->generation = owner.generation;
    frame->logicalTick = owner.completedTick; frame->qpcFrequency = owner.clock.frequency();
    frame->nativeInputMilliseconds = KHZ_ClientOwnerMillisecondsAt(owner.deadline.load());
    frame->currentSnapshotMessage = owner.currentSnapshotNumber;
    frame->nextSnapshotMessage = owner.publishedSnapshotNumber.load();
    return true;
}
bool KHZ_ClientOwnerOwnsAnimationState(const bgs_t* state) {
    return simulationThread && owner.active.load(std::memory_order_acquire) && state == &owner.game[0].bgs;
}
khz::GameplayRandomState* KHZ_ClientOwnerRandomState() {
    return (simulationThread || controlAccess) && owner.active.load(std::memory_order_acquire) ? &owner.randomState : nullptr;
}
bool KHZ_ClientOwnerRunning() { return owner.active.load(std::memory_order_acquire); }
std::uint64_t KHZ_ClientOwnerTickNumber() { return owner.tick.load(std::memory_order_relaxed); }
std::uint64_t KHZ_ClientOwnerInputDeadline() { return owner.deadline.load(std::memory_order_relaxed); }
unsigned KHZ_ClientOwnerMillisecondsAt(std::uint64_t counter) {
    const auto delta = counter >= owner.inputCounterEpoch ? counter - owner.inputCounterEpoch : 0;
    return owner.inputMillisecondsEpoch + unsigned(delta * 1000 / khz::frequency());
}
unsigned KHZ_ClientOwnerMilliseconds() { return KHZ_ClientOwnerMillisecondsAt(KHZ_ClientOwnerInputDeadline()); }
void KHZ_ClientOwnerRequestMainKey(int key, int down, unsigned milliseconds) {
    iassert(simulationThread);
    if (!owner.mainKeys.push({key, down, milliseconds})) failQueue("Main-thread navigation request queue exhausted");
}
bool KHZ_ClientOwnerPopMainKey(int* key, int* down, unsigned* milliseconds) {
    iassert(Sys_IsMainThread());
    MainKey value{}; if (!owner.mainKeys.pop(value)) return false;
    *key = value.key; *down = value.down; *milliseconds = value.milliseconds; return true;
}
void KHZ_ClientOwnerPublishKeyCatchers(int catchers) {
    iassert(Sys_IsMainThread());
    if (owner.active.load(std::memory_order_acquire)) owner.presentationUI[0].keyCatchers = catchers;
    owner.keyCatchers.store(catchers, std::memory_order_release);
}
void KHZ_ClientOwnerPublishInputCatchers(int catchers) {
    iassert(Sys_IsMainThread());
    owner.keyCatchers.store(catchers, std::memory_order_release);
}
int KHZ_ClientOwnerKeyCatchers() {
    const int frontend = owner.keyCatchers.load(std::memory_order_acquire);
    if (!simulationThread) return frontend;
    return (frontend & ~KEYCATCH_LOCATION_SELECTION) | (owner.locationSelection ? KEYCATCH_LOCATION_SELECTION : 0);
}
void KHZ_ClientOwnerSetLocationSelection(bool enabled) {
    iassert(simulationThread);
    owner.locationSelection = enabled;
}

clientActive_t (&KHZ_ClientActiveArray())[1] {
    return !simulationThread && !controlAccess && owner.active.load(std::memory_order_acquire) ? owner.presentationClient : khz_clients;
}
clientConnection_t (&KHZ_ClientConnectionArray())[1] {
    return !simulationThread && !controlAccess && owner.active.load(std::memory_order_acquire) ? owner.presentationConnection : khz_clientConnections;
}
clientUIActive_t (&KHZ_ClientUIArray())[1] {
    return !simulationThread && !controlAccess && owner.active.load(std::memory_order_acquire) ? owner.presentationUI : khz_clientUIActives;
}
clientStatic_t& KHZ_ClientStaticState() {
    return !simulationThread && !controlAccess && owner.active.load(std::memory_order_acquire) ? owner.presentationStatic : khz_cls;
}
cg_s (&KHZ_CGameArray())[1] { return simulationThread || controlAccess ? owner.game : khz_cgArray; }
cgs_t (&KHZ_CGameStaticArray())[1] { return simulationThread || controlAccess ? owner.gameStatic : khz_cgsArray; }
centity_s (&KHZ_CEntityArray())[1][1024] { return simulationThread || controlAccess ? owner.entities : khz_cg_entitiesArray; }
weaponInfo_s (&KHZ_CWeaponArray())[1][128] { return simulationThread || controlAccess ? owner.weapons : khz_cg_weaponsArray; }
float (&KHZ_CEntityOriginArray())[1][1024][3] { return simulationThread || controlAccess ? owner.origins : khz_cg_entityOriginArray; }

bool KHZ_ClientOwnerCanAttemptStart() {
    iassert(Sys_IsMainThread());
    return !owner.active.load(std::memory_order_acquire) && !owner.thread && khz::counter() >= owner.nextStartAttempt &&
        khz_clientUIActives[0].connectionState == CA_ACTIVE && khz_clientUIActives[0].cgameInitialized &&
        !khz_cgArray[0].isLoading && khz_cgArray[0].snap && khz_cgArray[0].nextSnap;
}
bool KHZ_ClientOwnerStart(std::uint64_t session, std::uint64_t generation, const KHZ_ClientOwnerHooks& hooks) {
    iassert(Sys_IsMainThread());
#ifdef KISAK_DEDICATED
    return false;
#else
    if (!KHZ_ClientOwnerCanAttemptStart()) return false;
    ++owner.startAttempts;
    std::snprintf(owner.startStage, sizeof(owner.startStage), "prerequisites");
    if (owner.active.load() || owner.thread || !session || !generation || !hooks.pumpTransport || !hooks.tick
        || khz_clientUIActives[0].connectionState != CA_ACTIVE || !khz_clientUIActives[0].cgameInitialized
        || khz_cgArray[0].isLoading || !khz_cgArray[0].snap || !khz_cgArray[0].nextSnap)
        return failStart("Active initialized CG snapshot pair is not ready");
    owner.randomState = KHZ_GetGameplayRandomState();
    owner.session = session; owner.generation = generation; owner.hooks = hooks;
    owner.map = khz::captureCollisionMapToken(session, generation);
    if (!owner.map.models || !owner.map.planes) return failStart("Loaded collision map has no models or planes");
    owner.phase.store(Phase::Starting); owner.stop.store(false); owner.park.store(false); owner.reasonClaimed.store(false);
    owner.resumeControls.store(false);
    owner.resumeCheckpoint.store(false); owner.parkedTaskPhase.store(ParkedTaskPhase::Idle);
    owner.checkpointParkRequested.store(false);
    owner.fault.store(false);
    owner.assetRebuild.store(false);
    owner.mainServerCommand.store(false); owner.mapLifecycle.store(false);
    owner.pendingServerCommand = 0; owner.pendingAssetMetadata = false; owner.pendingFinishesFragment = false; owner.refreshIncoming = false;
    owner.fragmentIndex = -1; owner.fragmentLast = 0; ownerBigConfigString[0] = 0;
    owner.fpX87.store(0); owner.fpMxcsr.store(0); owner.fpConfigured.store(false);
    owner.reason[0] = 0; owner.faultReason[0] = 0; owner.errorCode = 0;
    owner.tick.store(0); owner.deadline.store(0); owner.consumedTick = owner.completedTick = 0;
    owner.ticks.store(0); owner.snapshotsAccepted.store(0); owner.eventsWritten.store(0); owner.eventsRead.store(0);
    owner.poseTicks.store(0); owner.poseInitialized = false;
    owner.queueFailures.store(0); owner.parks.store(0); owner.lateMaximum.store(0); owner.debtMaximum.store(0); owner.aimStaleTicks.store(0);
    presentationFrame = nullptr;
    owner.presentationSnapshotsSkipped.store(0); owner.presentationSnapshotNumber.store(0);
    // Reclaim journals only after the previous owner has been joined.
    VisualEvent oldEvent; while (owner.events.pop(oldEvent)) {}
    ReliableCommand oldCommand; while (owner.reliable.pop(oldCommand)) {}
    ServerCommand oldServerCommand; while (owner.serverCommands.pop(oldServerCommand)) {}
    MainKey oldMainKey; while (owner.mainKeys.pop(oldMainKey)) {}
    if (!KHZ_IsHeadless() && khz_cls.rendererStarted) { R_SyncRenderThread(); R_WaitWorkerCmds(); }
    owner.presentationClient[0] = khz_clients[0];
    owner.presentationConnection[0] = khz_clientConnections[0];
    owner.presentationUI[0] = khz_clientUIActives[0];
    owner.keyCatchers.store(khz_clientUIActives[0].keyCatchers);
    owner.locationSelection = khz_cgArray[0].predictedPlayerState.locationSelectionInfo != 0;
    owner.presentationStatic = khz_cls;
    auto& mirror = owner.presentationClient[0];
    if (mirror.skelMemoryStart) {
        const auto offset = mirror.skelMemoryStart - khz_clients[0].skelMemory;
        mirror.skelMemoryStart = offset >= 0 && offset < sizeof(mirror.skelMemory) ? mirror.skelMemory + offset : mirror.skelMemory;
    }
    owner.game[0] = khz_cgArray[0];
    owner.gameStatic[0] = khz_cgsArray[0];
    auto& game = owner.game[0];
    copySnapshot(game.activeSnapshots[0], *khz_cgArray[0].snap);
    copySnapshot(game.activeSnapshots[1], *khz_cgArray[0].nextSnap);
    game.snap = &game.activeSnapshots[0]; game.nextSnap = &game.activeSnapshots[1];
    std::memcpy(owner.entities, khz_cg_entitiesArray, sizeof(owner.entities));
    std::memcpy(owner.weapons, khz_cg_weaponsArray, sizeof(owner.weapons));
    std::memcpy(owner.origins, khz_cg_entityOriginArray, sizeof(owner.origins));
    // Owner arithmetic never enters mutable visual XAnim/DObj callbacks.
    game.bgs.CreateDObj = nullptr; game.bgs.GetDObj = nullptr; game.bgs.SafeDObjFree = nullptr;
    game.bgs.AllocXAnim = nullptr; game.bgs.AttachWeapon = nullptr;
    game.bgs.animScriptData.playSoundAlias = KHZ_ClientOwnerQueueAnimationSound;
    for (auto& info : game.bgs.clientinfo) info.pXAnimTree = nullptr;
    owner.processedSnapshot = khz_cgsArray[0].processedSnapshotNum;
    owner.currentSnapshotNumber = owner.processedSnapshot;
    owner.copiedServerCommand = khz_cgsArray[0].serverCommandSequence;
    owner.deliveredServerCommand = owner.copiedServerCommand;
    owner.publishedServerCommand.store(owner.copiedServerCommand);
    owner.publishedSnapshotNumber.store(owner.processedSnapshot);
    owner.presentationSnapshotNumber.store(owner.processedSnapshot);
    owner.publishedSnapshotTime.store(game.nextSnap->serverTime);
    const int presentationStartSnapshot = owner.processedSnapshot;
    const int presentationStartTime = game.nextSnap->serverTime;
    owner.collisionBuild = {};
    owner.aim = *AimAssist_GetGlobals(0);
    std::snprintf(owner.startStage, sizeof(owner.startStage), "pose_prepare");
    if (!KHZ_ClientPosePrepare(&game, &owner.gameStatic[0], owner.entities[0], game.nextSnap))
        return failStart(KHZ_ClientPoseFailureReason()[0] ? KHZ_ClientPoseFailureReason() : "Native gameplay body/model/animation preparation rejected the accepted snapshot");
    std::snprintf(owner.startStage, sizeof(owner.startStage), "visibility_prepare");
    if (!KHZ_ClientVisibilityPrepare(&game, &owner.gameStatic[0])) {
        KHZ_ClientPoseRelease(); return failStart(KHZ_ClientVisibilityFailureReason()[0] ? KHZ_ClientVisibilityFailureReason() : "Native gameplay FX preparation or active-state migration rejected the world");
    }
    owner.privateDomainsReady = true;
    std::snprintf(owner.startStage, sizeof(owner.startStage), "thread_create");
    owner.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    owner.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    owner.parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    owner.parkedTaskDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!owner.wake || !owner.ready || !owner.parked || !owner.parkedTaskDone) {
        KHZ_ClientVisibilityRelease(); KHZ_ClientPoseRelease(); owner.privateDomainsReady = false;
        closeHandle(owner.wake); closeHandle(owner.ready); closeHandle(owner.parked); closeHandle(owner.parkedTaskDone); return failStart("Could not create client owner wake/ready/park events");
    }
    owner.active.store(true, std::memory_order_release);
    owner.thread = CreateThread(nullptr, 0, threadMain, nullptr, 0, nullptr);
    if (!owner.thread) {
        owner.active.store(false); closeHandle(owner.wake); closeHandle(owner.ready); closeHandle(owner.parked); closeHandle(owner.parkedTaskDone);
        KHZ_ClientVisibilityRelease(); KHZ_ClientPoseRelease(); owner.privateDomainsReady = false;
        return failStart("Could not create client simulation thread");
    }
    if (WaitForSingleObject(owner.ready, 5000) != WAIT_OBJECT_0) {
        // Keep ownership/lifetime pinned if the OS has not started the owner.
        // A caller must not unload a map on a failed acknowledgement.
        KHZ_ClientOwnerRequestPark("Client simulation thread did not acknowledge startup"); return false;
    }
    std::snprintf(owner.startStage, sizeof(owner.startStage), "running");
    // Native startup advertises the raw transport frontier, which can be ahead
    // of its installed snapshot pair after a reload. The presentation facade
    // now advertises only accepted owner publications. Rebase this availability
    // cursor once at the reader-domain handoff; retain both native snapshots
    // and their processed cursor. Every subsequent publication is monotonic.
    owner.startupTransportSnapshot = khz_cgArray[0].latestSnapshotNum;
    khz_cgArray[0].latestSnapshotNum = presentationStartSnapshot;
    khz_cgArray[0].latestSnapshotTime = presentationStartTime;
    khz_cgArray[0].bgs.latestSnapshotTime = khz_cgArray[0].latestSnapshotTime;
    owner.nextStartAttempt = 0;
    return true;
#endif
}
void KHZ_ClientOwnerRequestPark(const char* reason) {
    if (!owner.active.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (owner.reasonClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::snprintf(owner.reason, sizeof(owner.reason), "%s", reason ? reason : "Lifecycle transition");
        owner.park.store(true, std::memory_order_release);
    }
    if (owner.wake) SetEvent(owner.wake);
}
bool KHZ_ClientOwnerPark(unsigned timeout) {
    iassert(Sys_IsMainThread());
    if (!owner.active.load(std::memory_order_acquire)) return true;
    KHZ_ClientOwnerRequestPark("Main-thread lifecycle barrier");
    struct ParkEvent {
        HANDLE handle;
        bool reset() { return ResetEvent(handle) != FALSE; }
        bool wait(unsigned milliseconds) {
            const auto result = WaitForSingleObject(handle, milliseconds);
            return result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT;
        }
        std::uint64_t now() { return GetTickCount64(); }
    } event{owner.parked};
    return khz::park_wait::acknowledged(owner.phase, owner.park, Phase::Running,
        Phase::Parking, Phase::Parked, Phase::Fault, event, timeout);
}
void KHZ_ClientOwnerRequestAssetRebuild(const char* reason) {
    owner.assetRebuild.store(true, std::memory_order_release);
    KHZ_ClientOwnerRequestPark(reason);
}
bool KHZ_ClientOwnerNeedsAssetRebuild() { return owner.assetRebuild.load(std::memory_order_acquire); }
KHZ_ClientMainBarrierResult KHZ_ClientOwnerServiceMainBarrier() {
    iassert(Sys_IsMainThread());
    if (!owner.active.load(std::memory_order_acquire) || owner.phase.load() != Phase::Parked || controlAccess)
        return KHZ_ClientMainBarrierResult::None;
    if (owner.mapLifecycle.load(std::memory_order_acquire)) return KHZ_ClientMainBarrierResult::MapLifecycle;
    const bool reliable = owner.mainServerCommand.load(std::memory_order_acquire);
    const bool refresh = owner.assetRebuild.load(std::memory_order_acquire) || owner.pendingAssetMetadata;
    if (!reliable && !refresh) return KHZ_ClientMainBarrierResult::None;
    KHZ_DvarScope synchronization;
    if (refresh && !KHZ_IsHeadless() && owner.presentationStatic.rendererStarted) { R_SyncRenderThread(); R_WaitWorkerCmds(); }
    const int through = reliable ? owner.pendingServerCommand : owner.copiedServerCommand;
    auto& mainGame = khz_cgArray[0]; auto& mainStatic = khz_cgsArray[0];
    iassert(bgs == nullptr); bgs = &mainGame.bgs;
    CG_ExecuteNewServerCommands(0, through);
    bgs = nullptr;
    if (mainStatic.serverCommandSequence != through)
        Com_Error(ERR_DROP, "Main reliable command barrier did not complete its ordered prefix");
    // These are barrier-owned config/immutable asset metadata only. Network
    // snapshot/input histories and mutable owner CG/XAnim/FX are never copied.
    khz_clients[0].gameState = owner.presentationClient[0].gameState;
    khz_clients[0].serverId = owner.presentationClient[0].serverId;
    std::memcpy(khz_cls.mapCenter, owner.presentationStatic.mapCenter, sizeof(khz_cls.mapCenter));
    auto& target = owner.gameStatic[0];
    std::memcpy(target.gametype, mainStatic.gametype, sizeof(target.gametype));
    std::memcpy(target.szHostName, mainStatic.szHostName, sizeof(target.szHostName));
    std::memcpy(target.mapname, mainStatic.mapname, sizeof(target.mapname));
    target.maxclients = mainStatic.maxclients; target.gameEndTime = mainStatic.gameEndTime;
    target.voteTime = mainStatic.voteTime; target.voteYes = mainStatic.voteYes; target.voteNo = mainStatic.voteNo;
    std::memcpy(target.voteString, mainStatic.voteString, sizeof(target.voteString));
    std::memcpy(target.gameModels, mainStatic.gameModels, sizeof(target.gameModels));
    std::memcpy(target.fxs, mainStatic.fxs, sizeof(target.fxs));
    target.smokeGrenadeFx = mainStatic.smokeGrenadeFx; target.holdBreathParams = mainStatic.holdBreathParams;
    if (refresh) {
        const auto* accepted = owner.refreshIncoming ? &owner.incoming : owner.game[0].nextSnap;
        if (!KHZ_ClientPoseRefresh(&target, accepted))
            Com_Error(ERR_DROP, "Native gameplay pose cache refresh rejected metadata: %s", KHZ_ClientPoseFailureReason());
        if (!KHZ_ClientVisibilityRefresh(&target))
            Com_Error(ERR_DROP, "Native gameplay visibility cache refresh rejected metadata: %s", KHZ_ClientVisibilityFailureReason());
        ++owner.assetRefreshes;
    }
    if (reliable) {
        owner.copiedServerCommand = through; target.serverCommandSequence = through;
        khz_clientConnections[0].lastExecutedServerCommand = through;
        // Native main CL assembled the final fragment and installed its value.
        // The worker's partial string is no longer a live transaction.
        if (owner.pendingFinishesFragment) { owner.fragmentIndex = -1; ownerBigConfigString[0] = 0; }
        ++owner.mainServerBarriers;
    }
    owner.pendingServerCommand = 0; owner.pendingAssetMetadata = false; owner.pendingFinishesFragment = false; owner.refreshIncoming = false;
    owner.mainServerCommand.store(false, std::memory_order_release);
    owner.assetRebuild.store(false, std::memory_order_release);
    return KHZ_ClientMainBarrierResult::Resume;
}
bool KHZ_ClientOwnerBeginControls() {
    iassert(Sys_IsMainThread());
    if (controlAccess || !owner.active.load(std::memory_order_acquire) || owner.phase.load() != Phase::Parked
        || !owner.park.load(std::memory_order_acquire)
        || owner.parkedTaskPhase.load(std::memory_order_acquire) != ParkedTaskPhase::Idle) return false;
    controlAccess = true;
    return true;
}
void KHZ_ClientOwnerEndControls() {
    iassert(Sys_IsMainThread());
    controlAccess = false;
}
bool KHZ_ClientOwnerControlsActive() { return controlAccess; }
const char* KHZ_ClientOwnerBarrierFailure() {
    iassert(Sys_IsMainThread());
    static char result[512];
    const auto phase = owner.phase.load(std::memory_order_acquire);
    const auto event = owner.parked ? WaitForSingleObject(owner.parked, 0) : WAIT_FAILED;
    std::snprintf(result, sizeof(result),
        "phase=%s active=%u requested=%u event=%lu fault=%u stop=%u control=%u task=%u native_pending=%u",
        phaseName(phase), unsigned(owner.active.load(std::memory_order_acquire)),
        unsigned(owner.park.load(std::memory_order_acquire)), static_cast<unsigned long>(event),
        unsigned(owner.fault.load(std::memory_order_acquire)), unsigned(owner.stop.load(std::memory_order_acquire)),
        unsigned(controlAccess), unsigned(owner.parkedTaskPhase.load(std::memory_order_acquire)),
        unsigned(KHZ_ClientOwnerNativeBarrierPending()));
    return result;
}
bool KHZ_ClientOwnerNativeBarrierPending() {
    iassert(Sys_IsMainThread());
    return owner.active.load(std::memory_order_acquire) &&
        (owner.mainServerCommand.load(std::memory_order_acquire) ||
         owner.assetRebuild.load(std::memory_order_acquire) ||
         owner.mapLifecycle.load(std::memory_order_acquire));
}
bool KHZ_ClientOwnerResumeControls(unsigned timeout) {
    iassert(Sys_IsMainThread());
    if (controlAccess || !owner.active.load(std::memory_order_acquire) || owner.phase.load() != Phase::Parked
        || owner.fault.load(std::memory_order_acquire) || owner.stop.load(std::memory_order_acquire)
        || owner.parkedTaskPhase.load(std::memory_order_acquire) != ParkedTaskPhase::Idle
        || KHZ_ClientOwnerNativeBarrierPending()) return false;
    // Only the acknowledged park permits frontend state to be handed back.
    // Network clocks/history remain untouched, including outstanding tick debt.
    khz_clientUIActives[0].keyCatchers = owner.keyCatchers.load(std::memory_order_acquire);
    owner.locationSelection = owner.game[0].predictedPlayerState.locationSelectionInfo != 0;
    owner.reason[0] = 0; owner.reasonClaimed.store(false, std::memory_order_release);
    owner.checkpointParkRequested.store(false, std::memory_order_release);
    ResetEvent(owner.ready); ResetEvent(owner.parked);
    owner.resumeControls.store(true, std::memory_order_release);
    owner.park.store(false, std::memory_order_release);
    SetEvent(owner.wake);
    return WaitForSingleObject(owner.ready, timeout) == WAIT_OBJECT_0;
}
void KHZ_ClientOwnerStop() {
    KHZ_ClientCaptureInvalidateLifecycle();
    iassert(Sys_IsMainThread());
    if (!owner.thread) {
        if (owner.privateDomainsReady) { KHZ_ClientVisibilityRelease(); KHZ_ClientPoseRelease(); owner.privateDomainsReady = false; }
        return;
    }
    controlAccess = false;
    bgs = nullptr; // also clears a native main barrier abandoned by Com_Error
    // No timeout is allowed to release live CM/assets. Root's lifecycle caller
    // must respect a failed Park result and retain the old map until Stop joins.
    owner.phase.store(Phase::Stopping); owner.stop.store(true, std::memory_order_release);
    SetEvent(owner.wake);
    WaitForSingleObject(owner.thread, INFINITE);
    khz_cls.rendererStarted = owner.presentationStatic.rendererStarted;
    khz_cls.soundStarted = owner.presentationStatic.soundStarted;
    khz_cls.uiStarted = owner.presentationStatic.uiStarted;
    khz_cls.vidConfig = owner.presentationStatic.vidConfig;
    closeHandle(owner.thread); closeHandle(owner.wake); closeHandle(owner.ready); closeHandle(owner.parked); closeHandle(owner.parkedTaskDone);
    owner.active.store(false, std::memory_order_release); owner.phase.store(Phase::Stopped);
    owner.nativeConnection.store(CA_DISCONNECTED, std::memory_order_release);
    owner.nativeCgameInitialized.store(false, std::memory_order_release);
    owner.parkedTaskPhase.store(ParkedTaskPhase::Idle, std::memory_order_release);
    presentationFrame = nullptr;
    if (owner.privateDomainsReady) { KHZ_ClientVisibilityRelease(); KHZ_ClientPoseRelease(); owner.privateDomainsReady = false; }
}
bool KHZ_ClientOwnerNeedsMainThread() { return owner.active.load(std::memory_order_acquire) && owner.park.load(std::memory_order_acquire); }
bool KHZ_ClientOwnerHadFault() { return owner.fault.load(std::memory_order_acquire); }
int KHZ_ClientOwnerErrorCode() { return owner.fault.load(std::memory_order_acquire) ? owner.errorCode : 0; }
std::uint64_t KHZ_ClientOwnerGeneration() { return owner.active.load(std::memory_order_acquire) ? owner.generation : 0; }
const char* KHZ_ClientOwnerParkReason() {
    if (owner.fault.load(std::memory_order_acquire)) return owner.faultReason;
    return KHZ_ClientOwnerNeedsMainThread() ? owner.reason : "";
}
bool KHZ_ClientOwnerCatchError(int code, const char* format, va_list arguments) {
    if (!simulationThread) return false;
    if (!guardedCallback) TerminateProcess(GetCurrentProcess(), 1); // no cross-thread longjmp is valid
    owner.errorCode = code;
    std::vsnprintf(owner.faultReason, sizeof(owner.faultReason), format ? format : "Client simulation error", arguments);
    owner.faultReason[sizeof(owner.faultReason) - 1] = 0;
    owner.reasonClaimed.store(true, std::memory_order_relaxed);
    owner.fault.store(true, std::memory_order_release);
    owner.park.store(true, std::memory_order_release);
    // The native error recovery jumps across callback C++ scopes. Release this
    // thread's recursive dvar lifetime scopes before parking/rethrowing on main.
    KHZ_ControlsErrorUnwind();
    longjmp(callbackJump, 1);
}

void KHZ_ClientOwnerProcessSnapshots() {
    iassert(simulationThread);
    if (!owner.poseInitialized) {
        if (!KHZ_ClientPoseAccept(owner.game[0].nextSnap)) {
            KHZ_ClientOwnerRequestAssetRebuild("Initial accepted player model requires parked gameplay pose preparation"); return;
        }
        owner.poseInitialized = true;
    }
    int latest = 0, latestTime = 0;
    CL_GetCurrentSnapshotNumber(0, &latest, &latestTime);
    if (latest < owner.processedSnapshot) {
        owner.mapLifecycle.store(true, std::memory_order_release);
        KHZ_ClientOwnerRequestPark("Snapshot sequence reset requires a parked map/restart transition"); return;
    }
    // The transport callback may accept a bounded burst; process accepted
    // state in order. Retain network history rather than moving the cursor past
    // unread snapshots. The caller pumps the owner again on its next pass.
    for (unsigned count = 0; owner.processedSnapshot < latest && count < 64 && !owner.park.load(); ++count) {
        const int number = owner.processedSnapshot + 1;
        if (!CL_GetSnapshot(0, number, &owner.incoming)) { owner.processedSnapshot = number; continue; }
        if (!validSnapshot(owner.incoming)) { KHZ_ClientOwnerRequestPark("Invalid accepted snapshot cannot be installed in the gameplay owner"); return; }
        if (owner.incoming.serverTime < owner.game[0].nextSnap->serverTime ||
            ((owner.incoming.snapFlags ^ owner.game[0].nextSnap->snapFlags) & 4)) {
            owner.mapLifecycle.store(true, std::memory_order_release);
            KHZ_ClientOwnerRequestPark("Map/restart snapshot requires main-thread asset and visual lifetime barrier"); return;
        }
        // Reliable config/model updates precede model preflight and acceptance,
        // matching native CG_SetNextSnap's ordering.
        publishServerCommands(owner.incoming.serverCommandSequence);
        if (owner.park.load()) return;
        if (!KHZ_ClientPoseAccepts(&owner.incoming)) {
            owner.refreshIncoming = true;
            KHZ_ClientOwnerRequestAssetRebuild("Accepted player model or vehicle requires parked gameplay pose preparation"); return;
        }
        acceptGameplaySnapshot(number, owner.incoming);
        if (owner.park.load()) return;
        owner.processedSnapshot = number;
    }
}
bool KHZ_ClientOwnerBeginCollisionFrame() {
    iassert(simulationThread);
    KHZ_UnbindPredictionCollisionFrame();
    auto& game = owner.game[0];
    if (!khz::buildCollisionFrame(owner.collision, owner.collisionBuild, owner.map, game.snap, game.nextSnap,
            game.time, owner.tick.load(std::memory_order_relaxed), nullptr)
        || !KHZ_BindPredictionCollisionFrame(&owner.collision, owner.map)) {
        KHZ_ClientOwnerRequestPark("Incomplete snapshot collision world has no valid worker fallback"); return false;
    }
    return true;
}
void KHZ_ClientOwnerPublishPrediction() {
    iassert(simulationThread);
    const auto fp = khz::gameplayFpEnvironment();
    owner.fpX87.store(fp.x87Control, std::memory_order_relaxed);
    owner.fpMxcsr.store(fp.mxcsr, std::memory_order_relaxed);
    const bool fpMatches = khz::isGameplayFpEnvironment(fp);
    owner.fpConfigured.store(fpMatches, std::memory_order_release);
    if (!fpMatches) Com_Error(ERR_DROP, "Gameplay floating-point environment changed during simulation");
    if (!checkpointContext) KHZ_ClientCaptureTick(owner.currentSnapshotNumber, owner.publishedSnapshotNumber.load(std::memory_order_relaxed));
    auto& game = owner.game[0]; const auto& client = khz_clients[0];
    auto& output = owner.prediction.writable();
    output.session = owner.session; output.generation = owner.generation; output.tick = owner.tick.load(std::memory_order_relaxed);
    output.time = game.time; output.oldTime = game.oldTime; output.commandNumber = client.cmdNumber;
    output.serverId = client.serverId; output.connectionState = khz_clientUIActives[0].connectionState;
    owner.nativeConnection.store(khz_clientUIActives[0].connectionState, std::memory_order_release);
    owner.nativeCgameInitialized.store(khz_clientUIActives[0].cgameInitialized != 0, std::memory_order_release);
    output.latestSnapshotNumber = owner.publishedSnapshotNumber.load(std::memory_order_relaxed);
    output.latestSnapshotTime = owner.publishedSnapshotTime.load(std::memory_order_relaxed);
    output.currentSnapshotNumber = owner.currentSnapshotNumber;
    copySnapshot(output.currentWorld, *game.snap); copySnapshot(output.nextWorld, *game.nextSnap);
    output.realtime = khz_cls.realtime; output.frametime = khz_cls.frametime;
    output.authoritative = client.snap; output.player = game.predictedPlayerState;
    output.playerEntity = game.predictedPlayerEntity.nextState;
    CL_GetUserCmd(0, client.cmdNumber, &output.latestCommand);
    std::memcpy(output.predictedError, game.predictedError, sizeof(output.predictedError));
    output.predictedErrorTime = game.predictedErrorTime; output.stepViewStart = game.stepViewStart; output.stepViewChange = game.stepViewChange;
    std::memcpy(output.kickAVel, game.kickAVel, sizeof(output.kickAVel));
    std::memcpy(output.kickAngles, game.kickAngles, sizeof(output.kickAngles));
    std::memcpy(output.offsetAngles, game.offsetAngles, sizeof(output.offsetAngles));
    std::memcpy(output.vGunSpeed, game.vGunSpeed, sizeof(output.vGunSpeed));
    std::memcpy(output.vGunOffset, game.vGunOffset, sizeof(output.vGunOffset));
    std::memcpy(output.selectedLocation, game.selectedLocation, sizeof(output.selectedLocation));
    output.zoomSensitivity = game.zoomSensitivity; output.adsSensitivity = game.shellshock.sensitivity;
    output.weaponSelect = game.weaponSelect; output.weaponSelectTime = game.weaponSelectTime; output.equippedOffHand = game.equippedOffHand;
    output.weaponLatestPrimaryIdx = game.weaponLatestPrimaryIdx;
    output.invalidCmdHintType = game.invalidCmdHintType; output.invalidCmdHintTime = game.invalidCmdHintTime;
    output.ammoFadeTime = game.ammoFadeTime; output.lastActionSlotTime = game.lastActionSlotTime;
    output.cursorHintIcon = game.cursorHintIcon; output.cursorHintTime = game.cursorHintTime; output.proneBlockedEndTime = game.proneBlockedEndTime;
    output.landTime = game.landTime; output.landChange = game.landChange;
    output.positionToADS = game.playerEntity.bPositionToADS;
    output.lastWeaponPosFrac = game.playerEntity.fLastWeaponPosFrac;
    copyAnimation(output.animation, game.bgs.clientinfo[game.predictedPlayerState.clientNum]);
    owner.prediction.publish();
    owner.completedTick = output.tick;
}
void KHZ_ClientOwnerConsumePresentation() {
    iassert(Sys_IsMainThread());
    if (controlAccess) return; // A gameplay control scope cannot select visual CG.
    if (!owner.active.load(std::memory_order_acquire)) return;
    const auto& frame = owner.prediction.readLatest();
    if (frame.session != owner.session || frame.generation != owner.generation || !frame.tick) return;
    presentationFrame = &frame;
    auto& client = owner.presentationClient[0]; auto& game = khz_cgArray[0];
    client.snap = frame.authoritative; client.serverTime = frame.time; client.oldServerTime = frame.oldTime;
    client.serverId = frame.serverId; client.cmdNumber = frame.commandNumber;
    client.cmds[frame.commandNumber & 127] = frame.latestCommand;
    owner.presentationUI[0].connectionState = static_cast<connstate_t>(frame.connectionState);
    owner.presentationStatic.realtime = frame.realtime; owner.presentationStatic.frametime = frame.frametime;
    game.oldTime = game.time; game.time = frame.time; game.frametime = (std::max)(0, game.time - game.oldTime);
    game.bgs.time = game.time; game.bgs.frametime = game.frametime;
    iassert(bgs == nullptr); bgs = &game.bgs;
    if (!KHZ_IsHeadless()) {
        // Superseded visual samples are intentionally skipped. Native CG sees
        // the accepted current/next pair from this complete publication, while
        // every gameplay snapshot and event was processed by its owner.
        auto& processed = khz_cgsArray[0].processedSnapshotNum;
        if (processed < frame.currentSnapshotNumber - 1) {
            owner.presentationSnapshotsSkipped.fetch_add(frame.currentSnapshotNumber - 1 - processed);
            processed = frame.currentSnapshotNumber - 1;
        }
        CG_ProcessSnapshots(0);
    } else {
        CG_ExecuteNewServerCommands(0, frame.nextWorld.serverCommandSequence);
    }
    owner.presentationSnapshotNumber.store(frame.latestSnapshotNumber, std::memory_order_release);
    game.predictedPlayerState = frame.player; game.predictedPlayerEntity.nextState = frame.playerEntity;
    game.predictedPlayerEntity.currentState = frame.playerEntity.lerp;
    Vec3Copy(frame.player.origin, game.predictedPlayerEntity.pose.origin);
    std::memcpy(game.predictedError, frame.predictedError, sizeof(game.predictedError));
    game.predictedErrorTime = frame.predictedErrorTime; game.stepViewStart = frame.stepViewStart; game.stepViewChange = frame.stepViewChange;
    std::memcpy(game.kickAVel, frame.kickAVel, sizeof(game.kickAVel));
    std::memcpy(game.kickAngles, frame.kickAngles, sizeof(game.kickAngles));
    std::memcpy(game.offsetAngles, frame.offsetAngles, sizeof(game.offsetAngles));
    std::memcpy(game.vGunSpeed, frame.vGunSpeed, sizeof(game.vGunSpeed));
    std::memcpy(game.vGunOffset, frame.vGunOffset, sizeof(game.vGunOffset));
    std::memcpy(game.selectedLocation, frame.selectedLocation, sizeof(game.selectedLocation));
    game.zoomSensitivity = frame.zoomSensitivity; game.shellshock.sensitivity = frame.adsSensitivity;
    game.weaponSelect = frame.weaponSelect; game.weaponSelectTime = frame.weaponSelectTime; game.equippedOffHand = frame.equippedOffHand;
    game.weaponLatestPrimaryIdx = frame.weaponLatestPrimaryIdx;
    game.invalidCmdHintType = static_cast<InvalidCmdHintType>(frame.invalidCmdHintType); game.invalidCmdHintTime = frame.invalidCmdHintTime;
    game.ammoFadeTime = frame.ammoFadeTime; game.lastActionSlotTime = frame.lastActionSlotTime;
    game.cursorHintIcon = frame.cursorHintIcon; game.cursorHintTime = frame.cursorHintTime; game.proneBlockedEndTime = frame.proneBlockedEndTime;
    game.landTime = frame.landTime; game.landChange = frame.landChange;
    game.playerEntity.bPositionToADS = frame.positionToADS;
    game.playerEntity.fLastWeaponPosFrac = frame.lastWeaponPosFrac;
    applyAnimation(game.bgs.clientinfo[frame.player.clientNum], frame.animation);
    VisualEvent event{};
    while (const auto* pending = owner.events.front()) {
        if (pending->tick > frame.tick) break;
        if (!owner.events.pop(event)) break;
        if (event.session != owner.session || event.generation != owner.generation) continue;
        visualReplay = true;
        if (event.kind == EventKind::Damage) CG_DamageFeedback(0, event.damageYaw, event.damagePitch, event.damageCount);
        else if (event.kind == EventKind::MenuNotify) CG_MenuShowNotify(0, event.event);
        else if (event.kind == EventKind::AnimationSound) {
            if (game.bgs.animScriptData.playSoundAlias) game.bgs.animScriptData.playSoundAlias(event.entityNumber, event.alias);
        } else if (event.entityNumber >= 0 && event.entityNumber < 1024) {
            auto& visual = event.entityNumber == frame.player.clientNum ? game.predictedPlayerEntity : khz_cg_entitiesArray[0][event.entityNumber];
            const auto saved = visual.nextState;
            float savedOrigin[3], savedAngles[3]; Vec3Copy(visual.pose.origin, savedOrigin); Vec3Copy(visual.pose.angles, savedAngles);
            visual.nextState = event.state; Vec3Copy(event.origin, visual.pose.origin); Vec3Copy(event.angles, visual.pose.angles);
            CG_EntityEvent(0, &visual, event.event);
            visual.nextState = saved; Vec3Copy(savedOrigin, visual.pose.origin); Vec3Copy(savedAngles, visual.pose.angles);
        }
        visualReplay = false; ++owner.eventsRead;
    }
    bgs = nullptr;
    owner.consumedTick = frame.tick;
}
bool KHZ_ClientPresentationSnapshotNumber(int* number, int* time) {
    if (simulationThread || controlAccess || !owner.active.load(std::memory_order_acquire)) return false;
    *number = presentationFrame ? presentationFrame->latestSnapshotNumber : owner.presentationSnapshotNumber.load();
    *time = presentationFrame ? presentationFrame->latestSnapshotTime : owner.presentationClient[0].snap.serverTime;
    return true;
}
bool KHZ_ClientPresentationSnapshot(int number, snapshot_s* output, bool* found) {
    if (simulationThread || controlAccess || !owner.active.load(std::memory_order_acquire)) return false;
    *found = false;
    if (!presentationFrame || !output) return true;
    if (number == presentationFrame->latestSnapshotNumber) { copySnapshot(*output, presentationFrame->nextWorld); *found = true; }
    else if (number == presentationFrame->currentSnapshotNumber) { copySnapshot(*output, presentationFrame->currentWorld); *found = true; }
    return true;
}
bool KHZ_ClientPresentationUserCommand(int number, usercmd_s* output, bool* found) {
    if (simulationThread || controlAccess || !owner.active.load(std::memory_order_acquire)) return false;
    const auto& mirror = owner.presentationClient[0];
    *found = number == mirror.cmdNumber;
    if (*found) *output = mirror.cmds[number & 127];
    return true;
}
bool KHZ_ClientPresentationServerCommand(int number, char* output, unsigned capacity, bool* found) {
    if (simulationThread || controlAccess || !owner.active.load(std::memory_order_acquire)) return false;
    *found = false;
    const auto* pending = owner.serverCommands.front();
    if (!pending || pending->number != number || !output || !capacity || std::strlen(pending->text) >= capacity) {
        // Native CG advances its cursor before this call. Returning a missing
        // command would silently commit that advance; popping first also loses
        // the next ordered command. Retain the journal and end this connection
        // through the native error path instead of resuming a poisoned prefix.
        ++owner.queueFailures;
        Com_Error(ERR_DROP, "Reliable presentation command mismatch: requested=%d front=%d presented=%d delivered=%d published=%d capacity=%u generation=%llu",
            number, pending ? pending->number : -1, khz_cgsArray[0].serverCommandSequence,
            owner.deliveredServerCommand,
            owner.publishedServerCommand.load(),
            capacity, owner.generation);
        return true;
    }
    ServerCommand command{};
    if (!owner.serverCommands.pop(command)) Com_Error(ERR_DROP, "Reliable presentation command vanished from its sole consumer");
    std::memcpy(output, command.text, std::strlen(command.text) + 1);
    owner.presentationConnection[0].serverCommandSequence = number;
    owner.deliveredServerCommand = number;
    *found = true; return true;
}
bool KHZ_ClientOwnerQueueReliable(const char* text) {
    if (simulationThread || !owner.active.load(std::memory_order_acquire)) return false;
    iassert(Sys_IsMainThread());
    if (!text || std::strlen(text) >= sizeof(ReliableCommand::text)) { failQueue("Reliable command exceeds bounded owner ingress"); return true; }
    ReliableCommand command{}; std::snprintf(command.text, sizeof(command.text), "%s", text);
    if (!owner.reliable.push(command)) failQueue("Reliable command owner ingress exhausted");
    return true;
}
bool KHZ_ClientOwnerTryQueueReliable(const char* text) {
    if (simulationThread || !owner.active.load(std::memory_order_acquire)) return false;
    iassert(Sys_IsMainThread());
    if (!text || std::strlen(text) >= sizeof(ReliableCommand::text)) return false;
    ReliableCommand command{}; std::snprintf(command.text, sizeof(command.text), "%s", text);
    return owner.reliable.push(command);
}
bool KHZ_ClientOwnerPopReliable(char* text, unsigned capacity) {
    iassert(simulationThread);
    ReliableCommand command{};
    if (!owner.reliable.pop(command)) return false;
    if (!text || !capacity || std::strlen(command.text) >= capacity) { failQueue("Reliable owner consumer supplied insufficient command storage"); return false; }
    std::memcpy(text, command.text, std::strlen(command.text) + 1); return true;
}
void KHZ_ClientOwnerQueueEntityEvent(const centity_s* entity, int eventCode) {
    if (!entity || !eventCode) return;
    iassert(simulationThread);
    KHZ_ClientVisibilityEntityEvent(entity, eventCode);
    auto event = eventHeader(EventKind::Entity);
    event.entityNumber = entity->nextState.number; event.state = entity->nextState; event.event = eventCode;
    Vec3Copy(entity->pose.origin, event.origin); Vec3Copy(entity->pose.angles, event.angles);
    queueEvent(event);
}
int KHZ_ClientOwnerQueueAnimationSound(int entity, snd_alias_list_t* alias) {
    iassert(simulationThread);
    auto event = eventHeader(EventKind::AnimationSound); event.entityNumber = entity; event.alias = alias; queueEvent(event); return 0;
}
void KHZ_ClientOwnerQueueMenuNotify(int menu) {
    iassert(simulationThread);
    auto event = eventHeader(EventKind::MenuNotify); event.event = menu; queueEvent(event);
}
bool KHZ_ClientOwnerQueuePlayerTransition(playerState_s* state, const transPlayerState_t* previous, int* previousSequence) {
    if (!simulationThread) return false;
    if (state->damageEvent != previous->damageEvent && state->damageCount) {
        auto event = eventHeader(EventKind::Damage);
        event.damageYaw = state->damageYaw; event.damagePitch = state->damagePitch; event.damageCount = state->damageCount; queueEvent(event);
    }
    int sequence = previous->eventSequence <= state->eventSequence + 64 ? previous->eventSequence : previous->eventSequence - 256;
    auto& entity = owner.game[0].predictedPlayerEntity;
    for (int i = state->eventSequence - 4; i < state->eventSequence; ++i) {
        if (i >= sequence || (i > sequence - 4 && state->events[i & 3] != previous->events[i & 3])) {
            entity.nextState.eventParm = static_cast<unsigned char>(state->eventParms[i & 3]);
            CG_EntityEvent(0, &entity, state->events[i & 3]);
        }
    }
    *previousSequence = sequence; return true;
}
bool KHZ_ClientOwnerReplayingVisualEvent() { return visualReplay; }
AimAssistGlobals* KHZ_ClientOwnerAimContext(int localClientNum, AimAssistGlobals* presentation) {
    iassert(localClientNum == 0);
    return simulationThread || controlAccess ? &owner.aim : presentation;
}
void KHZ_ClientOwnerPublishAimTargets(const AimAssistGlobals* presentation) {
    // Presentation computes its own HUD targets. Gameplay never imports these
    // targets or a render-advanced pose; the native private domain owns both.
    (void)presentation;
}
void KHZ_ClientOwnerReadAimTargets() {
    iassert(simulationThread);
    if (!KHZ_ClientPoseTick()) {
        owner.aim.screenTargetCount = 0; ++owner.aimStaleTicks;
        if (!owner.park.load()) KHZ_ClientOwnerRequestPark("Private gameplay pose/visibility could not complete this tick");
        return;
    }
    ++owner.poseTicks;
}
const char* KHZ_ClientOwnerStatusJSON() {
    iassert(Sys_IsMainThread());
    static char output[8192];
    char startFailure[512], bodyFailure[2048];
    jsonString(startFailure, sizeof(startFailure), owner.startFailureReason);
    jsonString(bodyFailure, sizeof(bodyFailure), KHZ_ClientBodyFailureReason());
    std::snprintf(output, sizeof(output), "{\"phase\":\"%s\",\"active\":%s,\"session\":%llu,\"generation\":%llu,\"tick\":%llu,\"ticks\":%llu,\"snapshots\":%llu,\"snapshot_backlog\":%u,\"events_written\":%llu,\"events_read\":%llu,\"event_backlog\":%zu,\"queue_failures\":%llu,\"parks\":%llu,\"maximum_lateness_us\":%llu,\"maximum_tick_debt\":%llu,\"aim_stale_ticks\":%llu,\"fp_configured\":%s,\"fp_x87_control\":%u,\"fp_mxcsr\":%u,\"presentation_snapshots_skipped\":%llu,\"presentation_snapshot_lag\":%d,\"pose_ticks\":%llu,\"private_domains\":%s,\"gameplay_body_models\":%u,\"start_attempts\":%llu,\"start_failures\":%llu,\"start_stage\":\"%s\",\"start_failure_stage\":\"%s\",\"start_failure_reason\":\"%s\",\"main_server_barriers\":%llu,\"asset_refreshes\":%llu,\"server_commands_published\":%d,\"server_commands_presented\":%d}",
        phaseName(owner.phase.load()), owner.active.load() ? "true" : "false", owner.session, owner.generation, owner.tick.load(),
        owner.ticks.load(), owner.snapshotsAccepted.load(), 0u, owner.eventsWritten.load(), owner.eventsRead.load(),
        static_cast<std::size_t>(owner.events.size()), owner.queueFailures.load(), owner.parks.load(), owner.lateMaximum.load(), owner.debtMaximum.load(), owner.aimStaleTicks.load(),
        owner.fpConfigured.load(std::memory_order_acquire) ? "true" : "false", owner.fpX87.load(), owner.fpMxcsr.load(), owner.presentationSnapshotsSkipped.load(), (std::max)(0, owner.publishedSnapshotNumber.load() - owner.presentationSnapshotNumber.load()), owner.poseTicks.load(), owner.privateDomainsReady ? "true" : "false", KHZ_ClientBodyCount(), owner.startAttempts, owner.startFailures, owner.startStage, owner.startFailureStage, startFailure, owner.mainServerBarriers, owner.assetRefreshes, owner.publishedServerCommand.load(), khz_cgsArray[0].serverCommandSequence);
    const auto used = std::strlen(output);
    std::uint64_t expiredMessages = 0, expiredScans = 0;
    KHZ_ClientSnapshotExpiryCounters(&expiredMessages, &expiredScans);
    // Extend the bounded object without exposing unescaped asset names.
    if (used && output[used - 1] == '}') std::snprintf(output + used - 1, sizeof(output) - used + 1,
        ",\"body_failure\":\"%s\",\"stock_character_scripts\":%u,\"stock_character_compositions\":%u,\"gameplay_projectile_models\":%u,\"gameplay_fx_migrated_bolts\":%u,\"expired_snapshot_messages\":%llu,\"expired_snapshot_scans\":%llu,\"native_connection\":%d,\"native_cgame_initialized\":%s,\"startup_transport_snapshot\":%d,\"presentation_snapshot_available\":%d,\"presentation_snapshot_scanned\":%d,\"owner_snapshot_accepted\":%d}",
        bodyFailure, KHZ_ClientBodyStockScripts(), KHZ_ClientBodyStockCompositions(),
        KHZ_ClientProjectileModelCount(), KHZ_ClientVisibilityMigratedBolts(), expiredMessages, expiredScans,
        owner.nativeConnection.load(std::memory_order_acquire), owner.nativeCgameInitialized.load(std::memory_order_acquire) ? "true" : "false",
        owner.startupTransportSnapshot, khz_cgArray[0].latestSnapshotNum, khz_cgsArray[0].processedSnapshotNum,
        owner.publishedSnapshotNumber.load(std::memory_order_acquire));
    return output;
}

namespace {
bool ownerCheckpointCapture(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity) {
    KHZ_ClientCheckpointFrame frame; frame.fixture = 1;
    if (!KHZ_ClientOwnerCheckpointObserve(&frame, reason, capacity)) return false;
    ownerCheckpoint.poseInitialized = owner.poseInitialized; ownerCheckpoint.locationSelection = owner.locationSelection;
    ownerCheckpoint.processedSnapshot = owner.processedSnapshot; ownerCheckpoint.currentSnapshotNumber = owner.currentSnapshotNumber;
    ownerCheckpoint.publishedSnapshotNumber = owner.publishedSnapshotNumber.load();
    ownerCheckpoint.publishedSnapshotTime = owner.publishedSnapshotTime.load();
    ownerCheckpoint.copiedServerCommand = owner.copiedServerCommand;
    ownerCheckpoint.publishedServerCommand = owner.publishedServerCommand.load();
    ownerCheckpoint.keyCatchers = owner.keyCatchers.load(); ownerCheckpoint.completedTick = owner.completedTick;
    ownerCheckpoint.randomState = owner.randomState; ownerCheckpoint.map = owner.map;
    ownerCheckpoint.collisionBuild = owner.collisionBuild;
    std::memcpy(&ownerCheckpoint.incoming, &owner.incoming, sizeof(ownerCheckpoint.incoming));
    std::memcpy(&ownerCheckpoint.presentation, presentationFrame, sizeof(ownerCheckpoint.presentation));
    ownerCheckpoint.captured = true; return true;
}
bool ownerCheckpointValidate(const KHZ_ClientCheckpointFrame& saved, char* reason, unsigned capacity) {
    KHZ_ClientCheckpointFrame current; current.fixture = saved.fixture;
    if (!ownerCheckpoint.captured || !KHZ_ClientOwnerCheckpointObserve(&current, reason, capacity)) return false;
    if (!khz::checkpoint::sameLifetime(saved, current) || ownerCheckpoint.map.models != owner.map.models ||
        ownerCheckpoint.map.planes != owner.map.planes || ownerCheckpoint.map.checksum != owner.map.checksum) {
        if (reason && capacity) std::snprintf(reason, capacity, "owner_checkpoint_map_or_session_identity_changed"); return false;
    }
    khz::DeadlineClock clock;
    if (!clock.rebase(khz::counter(), saved.qpcFrequency, saved.logicalTick)) {
        if (reason && capacity) std::snprintf(reason, capacity, "owner_checkpoint_clock_cannot_rebase"); return false;
    }
    return true;
}
bool ownerCheckpointRestore(const KHZ_ClientCheckpointFrame& saved, std::uint64_t epoch, char* reason, unsigned capacity) {
    if (!ownerCheckpointValidate(saved, reason, capacity) || !owner.clock.rebase(epoch, saved.qpcFrequency, saved.logicalTick)) return false;
    owner.inputCounterEpoch = epoch; owner.inputMillisecondsEpoch = saved.nativeInputMilliseconds;
    owner.tick.store(saved.logicalTick); owner.deadline.store(epoch); owner.completedTick = saved.logicalTick;
    owner.poseInitialized = ownerCheckpoint.poseInitialized; owner.locationSelection = ownerCheckpoint.locationSelection;
    owner.processedSnapshot = ownerCheckpoint.processedSnapshot; owner.currentSnapshotNumber = ownerCheckpoint.currentSnapshotNumber;
    owner.publishedSnapshotNumber.store(ownerCheckpoint.publishedSnapshotNumber);
    owner.publishedSnapshotTime.store(ownerCheckpoint.publishedSnapshotTime);
    owner.copiedServerCommand = ownerCheckpoint.copiedServerCommand;
    owner.publishedServerCommand.store(ownerCheckpoint.publishedServerCommand); owner.keyCatchers.store(ownerCheckpoint.keyCatchers);
    owner.randomState = ownerCheckpoint.randomState; owner.map = ownerCheckpoint.map;
    owner.collisionBuild = ownerCheckpoint.collisionBuild;
    std::memcpy(&owner.incoming, &ownerCheckpoint.incoming, sizeof(owner.incoming));
    // Both reader and writer are fenced. All three records acquire the saved
    // complete frame, so no pending future publication survives the rewind.
    for (auto& slot : predictionFrameStorage) std::memcpy(&slot, &ownerCheckpoint.presentation, sizeof(slot));
    return true;
}
void ownerCheckpointDiscard() { ownerCheckpoint.captured = false; }
}
void KHZ_ClientOwnerRegisterCheckpoint() {
    const KHZ_ClientCheckpointProvider provider{KHZ_ClientCheckpointDomain::OwnerBoundary, sizeof(ownerCheckpoint),
        ownerCheckpointCapture, ownerCheckpointValidate, ownerCheckpointRestore, ownerCheckpointDiscard};
    KHZ_ClientCheckpointRegister(provider);
}
bool KHZ_ClientOwnerCheckpointResetPresentation() {
    iassert(Sys_IsMainThread());
    if (!owner.active.load() || owner.phase.load() != Phase::Parked || controlAccess || owner.fault.load() ||
        !ownerCheckpoint.captured || owner.parkedTaskPhase.load() != ParkedTaskPhase::Idle) return false;
    if (!KHZ_IsHeadless() && owner.presentationStatic.rendererStarted) { R_SyncRenderThread(); R_WaitWorkerCmds(); }
    owner.presentationClient[0] = khz_clients[0]; owner.presentationConnection[0] = khz_clientConnections[0];
    owner.presentationUI[0] = khz_clientUIActives[0];
    const auto offset = owner.presentationClient[0].skelMemoryStart - khz_clients[0].skelMemory;
    owner.presentationClient[0].skelMemoryStart = owner.presentationClient[0].skelMemory + offset;
    owner.presentationStatic.realtime = khz_cls.realtime; owner.presentationStatic.frametime = khz_cls.frametime;
    owner.presentationStatic.realFrametime = khz_cls.realFrametime;
    std::memcpy(owner.presentationStatic.mapCenter, khz_cls.mapCenter, sizeof(khz_cls.mapCenter));
    presentationFrame = &owner.prediction.readLatest();
    owner.presentationSnapshotNumber.store(owner.currentSnapshotNumber);
    auto& visual = khz_cgArray[0]; auto& state = khz_cgsArray[0];
    state = owner.gameStatic[0]; state.processedSnapshotNum = owner.currentSnapshotNumber;
    state.serverCommandSequence = owner.copiedServerCommand;
    owner.deliveredServerCommand = owner.copiedServerCommand;
    copySnapshot(visual.activeSnapshots[0], *owner.game[0].snap);
    copySnapshot(visual.activeSnapshots[1], *owner.game[0].nextSnap);
    iassert(bgs == nullptr); bgs = &visual.bgs;
    if (!KHZ_IsHeadless()) {
        FX_KillAllEffects(0);
        CG_SetInitialSnapshot(0, &visual.activeSnapshots[0]);
        CG_SetNextSnap(0, &visual.activeSnapshots[1]);
    } else { visual.snap = &visual.activeSnapshots[0]; visual.nextSnap = &visual.activeSnapshots[1]; }
    bgs = nullptr;
    visual.time = owner.game[0].time; visual.oldTime = owner.game[0].oldTime;
    visual.bgs.time = visual.time; visual.bgs.frametime = 0;
    visual.latestSnapshotNum = owner.publishedSnapshotNumber.load(); visual.latestSnapshotTime = owner.publishedSnapshotTime.load();
    owner.consumedTick = 0;
    KHZ_ClientOwnerConsumePresentation();
    return true;
}
bool KHZ_ClientOwnerResumeCheckpoint(unsigned timeout) {
    iassert(Sys_IsMainThread());
    if (controlAccess || !owner.active.load() || owner.phase.load() != Phase::Parked || owner.fault.load() ||
        owner.stop.load() || owner.parkedTaskPhase.load() != ParkedTaskPhase::Idle || !KHZ_InputOwnerCheckpointParked()) return false;
    owner.reason[0] = 0; owner.reasonClaimed.store(false, std::memory_order_release);
    owner.checkpointParkRequested.store(false, std::memory_order_release);
    ResetEvent(owner.ready); ResetEvent(owner.parked);
    owner.resumeCheckpoint.store(true, std::memory_order_release); owner.park.store(false, std::memory_order_release);
    SetEvent(owner.wake);
    return WaitForSingleObject(owner.ready, timeout) == WAIT_OBJECT_0 && !owner.fault.load(std::memory_order_acquire);
}

#include "khz_equipment_owner.inl"
