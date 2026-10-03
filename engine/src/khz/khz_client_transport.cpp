#include <universal/q_shared.h>
#include "khz_client_transport.h"
#include "khz_client_owner.h"
#include "khz_client_input.h"
#include "khz_input_owner.h"
#include "khz_runtime.h"
#include "khz_controls.h"
#include "khz_screenshot.h"
#include "khz_client_checkpoint.h"
#include "khz_client_input_checkpoint.h"
#include "khz_host.h"
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <cgame_mp/cg_local_mp.h>
#include <qcommon/qcommon.h>
#include <qcommon/msg_mp.h>
#include <qcommon/net_chan_mp.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <publication.hpp>
#include <connectionless_policy.hpp>
#include <gfx_d3d/r_rendercmds.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <Windows.h>

namespace {
constexpr unsigned MaximumPayload = 0x20000;
struct FrontendPacket {
    netadr_t from{}, gameplayServer{};
    khz::connectionless::Owner owner = khz::connectionless::Owner::Frontend;
    std::uint64_t session = 0, generation = 0;
    int time = 0, size = 0;
    unsigned char data[16384]{};
};
khz::SpscQueue<FrontendPacket, 64> frontendPackets;
struct Payload { int size = 0, read = 0, bit = 0, lastEntity = 0, sequence = 0, acknowledge = 0; bool hasHeader = false; unsigned char data[MaximumPayload]{}; };
Payload lifecycle;
std::atomic<bool> lifecycleReady{false};
std::uint64_t generation = 0;
unsigned realtimeOffset = 0;
std::atomic<bool> checkpointFenced{false};
std::uint64_t fenceFixture = 0, fenceSession = 0, fenceGeneration = 0;
unsigned fenceDrained = 0;
struct TransportCheckpointImage {
    khz::checkpoint::Frame frame;
    std::uint64_t generation;
    unsigned realtimeOffset;
    bool captured;
};
TransportCheckpointImage transportImage;
bool transportReject(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text);
    return false;
}
bool transportQuiescent(const khz::checkpoint::Frame& frame, char* reason, unsigned capacity) {
    if (!KHZ_IsClientSimulationThread() || !KHZ_ClientOwnerCheckpointContext() ||
        !KHZ_ClientTransportCheckpointFenced(frame.fixture, frame.session, frame.generation))
        return transportReject(reason, capacity, "transport_checkpoint_requires_actual_owned_server_fence");
    if (lifecycleReady.load(std::memory_order_acquire) || frontendPackets.size() ||
        !NET_CheckpointQueuesQuiescent(NS_CLIENT1) || !KHZ_InputOwnerCheckpointParked())
        return transportReject(reason, capacity, "transport_checkpoint_native_or_input_queue_not_empty");
    return true;
}
bool transportCapture(const khz::checkpoint::Frame& frame, char* reason, unsigned capacity) {
    if (!transportQuiescent(frame, reason, capacity)) return false;
    transportImage.frame = frame; transportImage.generation = generation;
    transportImage.realtimeOffset = realtimeOffset; transportImage.captured = true;
    return true;
}
bool transportValidate(const khz::checkpoint::Frame& frame, char* reason, unsigned capacity) {
    if (!transportQuiescent(frame, reason, capacity)) return false;
    return transportImage.captured && khz::checkpoint::sameLifetime(frame, transportImage.frame) &&
        generation == transportImage.generation ? true :
        transportReject(reason, capacity, "transport_checkpoint_generation_changed");
}
bool transportRestore(const khz::checkpoint::Frame& frame, std::uint64_t, char* reason, unsigned capacity) {
    if (!transportValidate(frame, reason, capacity)) return false;
    realtimeOffset = transportImage.realtimeOffset;
    return true;
}
void transportDiscard() { transportImage.captured = false; }
bool drainCheckpointSocket(void*) {
    if (!KHZ_IsClientSimulationThread() || !KHZ_ClientOwnerCheckpointContext() ||
        !KHZ_HostCheckpointFenced(fenceFixture, fenceSession) ||
        lifecycleReady.load(std::memory_order_acquire) || frontendPackets.size() ||
        !KHZ_InputOwnerParkAcknowledged()) return false;
    static thread_local unsigned char storage[MaximumPayload];
    msg_t message; netadr_t from; MSG_Init(&message, storage, sizeof(storage));
    unsigned discarded = 0;
    while (NET_GetLoopPacket(NS_CLIENT1, &from, &message)) if (++discarded > 4096) return false;
    while (NET_GetClientPacket(&from, &message)) if (++discarded > 4096) return false;
    if (!NET_CheckpointQueuesQuiescent(NS_CLIENT1) || !KHZ_ClientInputCheckpointPrepareReleased()) return false;
    fenceDrained += discarded;
    checkpointFenced.store(true, std::memory_order_release); return true;
}
void dispatch(netadr_t from, msg_t& message) {
    if (message.cursize >= 4 && *reinterpret_cast<const unsigned*>(message.data) == ~0u) {
        FrontendPacket packet;
        if (message.cursize > sizeof(packet.data)) return;
        packet.owner = khz::connectionless::classify(message.data, message.cursize);
        packet.gameplayServer = CL_GetLocalClientConnection(0)->serverAddress;
        packet.session = KHZ_AcceptedClientSession(); packet.generation = KHZ_ClientOwnerGeneration();
        if (packet.owner != khz::connectionless::Owner::Frontend && !NET_CompareAdr(from, packet.gameplayServer)) return;
        packet.from = from; packet.time = KHZ_ClientOwnerMilliseconds(); packet.size = message.cursize;
        std::memcpy(packet.data, message.data, packet.size);
        if (!frontendPackets.push(packet)) KHZ_ClientOwnerRequestPark("Frontend network queue exhausted");
        else if (packet.owner != khz::connectionless::Owner::Frontend)
            // Do not accept a later gamestate before main applies this earlier
            // connection transition. The worker remains the sole receiver.
            KHZ_ClientOwnerRequestPark("Gameplay server connection control packet");
        return;
    }
    CL_PacketEvent(0, from, &message, KHZ_ClientOwnerMilliseconds());
}
void tick(std::uint64_t, std::uint64_t) {
    if (checkpointFenced.load(std::memory_order_acquire)) return;
    KHZ_DvarScope synchronization;
    cls.realtime = KHZ_ClientOwnerMilliseconds() - realtimeOffset; cls.frametime = 1;
    char command[1024];
    while (KHZ_ClientOwnerPopReliable(command, sizeof(command))) CL_AddReliableCommand(0, command);
    CL_CheckTimeout(0);
    CL_SetCGameTime(0);
    KHZ_ClientTick();
}
void resumeConnectionControls() {
    if (KHZ_ClientOwnerNativeBarrierPending()) return;
    if (!KHZ_ClientInputFreezeBindings()) Com_Error(ERR_DROP, "Gameplay binding exceeds bounded input storage");
    KHZ_InputOwnerSetGameplay(KHZ_ClientGameplayInputAllowed(), KHZ_ClientOwnerGeneration());
    KHZ_InputOwnerResume();
    if (!KHZ_ClientOwnerResumeControls()) Com_Error(ERR_DROP, "Server connection control barrier did not resume");
}
}
void KHZ_ClientTransportPump() {
    iassert(KHZ_IsClientSimulationThread());
    if (checkpointFenced.load(std::memory_order_acquire)) return;
    KHZ_DvarScope synchronization;
    FakeLag_Frame();
    static thread_local unsigned char storage[MaximumPayload];
    msg_t message; netadr_t from;
    MSG_Init(&message, storage, sizeof(storage));
    unsigned packets = 0;
    while (!KHZ_ClientOwnerNeedsMainThread() && packets++ < 256 && NET_GetLoopPacket(NS_CLIENT1, &from, &message)) dispatch(from, message);
    packets = 0;
    while (!KHZ_ClientOwnerNeedsMainThread() && packets++ < 256 && NET_GetClientPacket(&from, &message)) dispatch(from, message);
}
bool KHZ_ClientTransportDeferPayload(const msg_t* decoded, bool hasHeader) {
    if (!KHZ_IsClientSimulationThread()) return false;
    if (!decoded || decoded->cursize < 0 || decoded->cursize > MaximumPayload || decoded->readcount < 0 ||
        decoded->readcount > decoded->cursize || lifecycleReady.load(std::memory_order_acquire))
        Com_Error(ERR_DROP, "Client lifecycle payload storage exhausted");
    lifecycle.size = decoded->cursize; lifecycle.read = decoded->readcount;
    lifecycle.bit = decoded->bit; lifecycle.lastEntity = decoded->lastEntityRef;
    lifecycle.hasHeader = hasHeader;
    lifecycle.sequence = CL_GetLocalClientConnection(0)->serverMessageSequence;
    lifecycle.acknowledge = CL_GetLocalClientConnection(0)->reliableAcknowledge;
    std::memcpy(lifecycle.data, decoded->data, decoded->cursize);
    lifecycleReady.store(true, std::memory_order_release);
    KHZ_ClientOwnerRequestPark("Server lifecycle payload requires main-thread asset work");
    return true;
}
void KHZ_ClientTransportMainService() {
    iassert(Sys_IsMainThread());
    if (lifecycleReady.load(std::memory_order_acquire)) {
        KHZ_ClientSimulationShutdown();
        msg_t message; MSG_Init(&message, lifecycle.data, sizeof(lifecycle.data));
        message.cursize = lifecycle.size; message.readcount = lifecycle.read;
        message.bit = lifecycle.bit; message.lastEntityRef = lifecycle.lastEntity;
        lifecycleReady.store(false, std::memory_order_release);
        CL_GetLocalClientConnection(0)->serverMessageSequence = lifecycle.sequence;
        CL_GetLocalClientConnection(0)->reliableAcknowledge = lifecycle.acknowledge;
        if (!lifecycle.hasHeader || KHZ_ReadServerHeader(&message)) CL_ParseServerPayload(0, &message);
    }
    FrontendPacket packet;
    while (frontendPackets.pop(packet)) {
        msg_t message; MSG_Init(&message, packet.data, sizeof(packet.data)); message.cursize = packet.size;
        if (packet.owner != khz::connectionless::Owner::Frontend) {
            if (!NET_CompareAdr(packet.from, packet.gameplayServer)) continue;
            const bool running = KHZ_ClientOwnerRunning();
            if (running && packet.generation != KHZ_ClientOwnerGeneration()) continue;
            if (running && !KHZ_ClientOwnerPark()) Com_Error(ERR_DROP, "Server connection control barrier did not acknowledge");
            if (running && !KHZ_ClientOwnerBeginControls())
                Com_Error(ERR_DROP, "Server connection control barrier has no gameplay owner");
            if (!NET_CompareAdr(packet.from, CL_GetLocalClientConnection(0)->serverAddress) ||
                (running && packet.session != KHZ_AcceptedClientSession())) {
                if (KHZ_ClientOwnerControlsActive()) KHZ_ClientOwnerEndControls();
                if (running) resumeConnectionControls();
                continue;
            }
            // Asset/error callbacks select native presentation CG after Stop.
            // CL/connection/netchan storage is already the actual owner state
            // and is preserved across the join for stock handshake recovery.
            if (packet.owner == khz::connectionless::Owner::Lifecycle) {
                KHZ_ClientSimulationShutdown();
                R_SyncRenderThread();
            }
            const auto sessionBefore = KHZ_AcceptedClientSession();
            const auto serverBefore = CL_GetLocalClientGlobals(0)->serverId;
            CL_ConnectionlessPacket(0, packet.from, &message, packet.time);
            if (KHZ_ClientOwnerRunning()) {
                const bool retain = CL_GetLocalClientConnectionState(0) == CA_ACTIVE &&
                    clientUIActives[0].cgameInitialized && KHZ_AcceptedClientSession() == sessionBefore &&
                    CL_GetLocalClientGlobals(0)->serverId == serverBefore;
                if (KHZ_ClientOwnerControlsActive()) KHZ_ClientOwnerEndControls();
                if (!retain) KHZ_ClientSimulationShutdown();
                else resumeConnectionControls();
            }
            continue;
        }
        CL_ConnectionlessPacket(0, packet.from, &message, packet.time);
    }
    int key, down; unsigned time;
    while (KHZ_ClientOwnerPopMainKey(&key, &down, &time)) {
        const bool running = KHZ_ClientOwnerRunning();
        if (running && !KHZ_ClientOwnerPark()) Com_Error(ERR_DROP, "Input control barrier did not acknowledge");
        KHZ_InputOwnerPark();
        if (running && !KHZ_ClientOwnerBeginControls()) Com_Error(ERR_DROP, "Input control barrier has no gameplay owner");
        CL_KeyEvent(0, key, down, time);
        if (running) KHZ_ClientOwnerEndControls();
    }
}
void KHZ_ClientTransportReset() {
    iassert(!KHZ_ClientOwnerRunning());
    checkpointFenced.store(false, std::memory_order_release);
    lifecycleReady.store(false); FrontendPacket value; while (frontendPackets.pop(value)) {}
}
void KHZ_ClientTransportRegisterCheckpoint() {
    iassert(Sys_IsMainThread());
    const KHZ_ClientCheckpointProvider provider{khz::checkpoint::Domain::Transport, sizeof(transportImage),
        transportCapture, transportValidate, transportRestore, transportDiscard};
    if (!KHZ_ClientCheckpointRegister(provider)) Com_Error(ERR_FATAL, "Duplicate transport checkpoint provider");
    KHZ_ClientInputRegisterCheckpoint();
}
bool KHZ_ClientTransportCheckpointFenced(std::uint64_t fixture, std::uint64_t session, std::uint64_t mapGeneration) {
    return checkpointFenced.load(std::memory_order_acquire) && fixture == fenceFixture &&
        session == fenceSession && mapGeneration == fenceGeneration &&
        KHZ_HostCheckpointFenced(fixture, session);
}
bool KHZ_ClientTransportBeginCheckpointFence(std::uint64_t fixture, unsigned timeout) {
    iassert(Sys_IsMainThread());
    if (!fixture || !timeout || timeout > 5000 || !KHZ_ClientOwnerRunning() ||
        KHZ_ClientOwnerHadFault() || lifecycleReady.load(std::memory_order_acquire) || frontendPackets.size() ||
        !Dvar_GetBool("khz_benchmark")) return false;
    if (!KHZ_ClientOwnerParkCheckpoint(timeout)) return false;
    KHZ_InputOwnerPark(); KHZ_ClientOwnerConsumePresentation();
    const auto until = GetTickCount64() + timeout;
    while (!KHZ_InputOwnerParkAcknowledged() && GetTickCount64() < until) Sleep(1);
    if (!KHZ_InputOwnerParkAcknowledged()) return false;
    fenceFixture = fixture; fenceSession = KHZ_AcceptedClientSession();
    fenceGeneration = KHZ_ClientOwnerGeneration(); fenceDrained = 0;
    checkpointFenced.store(false, std::memory_order_release);
    if (!KHZ_HostCheckpointPause(fixture, fenceSession, timeout)) return false;
    // Give already submitted localhost datagrams a separate receipt pass after
    // both native senders stopped. Kernel and fake-lag queues are then drained
    // by their real sole consumer, with a hard bound and no gameplay dispatch.
    Sleep(2);
    return KHZ_ClientOwnerRunParkedTask(drainCheckpointSocket, nullptr, timeout) &&
        checkpointFenced.load(std::memory_order_acquire);
}
bool KHZ_ClientTransportEndCheckpointFence(std::uint64_t fixture, unsigned timeout) {
    iassert(Sys_IsMainThread());
    if (!checkpointFenced.load(std::memory_order_acquire) || fixture != fenceFixture ||
        !KHZ_HostCheckpointResume(fixture, timeout)) return false;
    checkpointFenced.store(false, std::memory_order_release);
    // The caller owns presentation/input checkpoint rebasing and the explicit
    // replay resume. Normal ResumeControls would reset the restored TLS state.
    return true;
}
void KHZ_ClientSimulationShutdown() {
    iassert(Sys_IsMainThread());
    KHZ_InputOwnerPark();
    KHZ_ClientOwnerStop();
    KHZ_ScreenshotClientDisconnect();
}
void KHZ_ClientSimulationService() {
    iassert(Sys_IsMainThread());
    KHZ_ClientTransportMainService();
    if (KHZ_ClientOwnerRunning()) {
        if (KHZ_ClientOwnerHadFault()) {
            char reason[256]; I_strncpyz(reason, KHZ_ClientOwnerParkReason(), sizeof(reason));
            const int code = KHZ_ClientOwnerErrorCode();
            KHZ_ClientSimulationShutdown();
            Com_Error(static_cast<errorParm_t>(code), "%s", reason);
        }
        if (KHZ_ClientOwnerNeedsMainThread()) {
            if (!KHZ_ClientOwnerPark()) Com_Error(ERR_DROP, "Client simulation barrier did not acknowledge");
            KHZ_ClientOwnerConsumePresentation();
            const auto barrier = KHZ_ClientOwnerServiceMainBarrier();
            if (barrier == KHZ_ClientMainBarrierResult::MapLifecycle) {
                KHZ_ClientSimulationShutdown();
                auto* game = CG_GetLocalClientGlobals(0);
                game->oldTime = game->time; game->time = CL_GetLocalClientGlobals(0)->serverTime;
                game->frametime = 0;
                iassert(bgs == nullptr); bgs = &game->bgs;
                CG_ProcessSnapshots(0); bgs = nullptr;
                return;
            }
            if (!KHZ_ClientOwnerRunning()) return;
            if (!KHZ_ClientInputFreezeBindings()) Com_Error(ERR_DROP, "Gameplay binding exceeds bounded input storage");
            const bool gameplay = KHZ_ClientGameplayInputAllowed();
            KHZ_InputOwnerSetGameplay(gameplay, KHZ_ClientOwnerGeneration()); KHZ_InputOwnerResume();
            if (!KHZ_ClientOwnerResumeControls()) Com_Error(ERR_DROP, "Client simulation barrier did not resume");
        }
        const bool gameplay = KHZ_ClientGameplayInputAllowed();
        KHZ_InputOwnerSetGameplay(gameplay, KHZ_ClientOwnerGeneration());
        KHZ_ClientOwnerPublishInputCatchers(clientUIActives[0].keyCatchers |
            (gameplay ? 0 : KEYCATCH_UI));
        KHZ_ClientOwnerConsumePresentation();
        return;
    }
    if (com_sv_running->current.enabled || CL_GetLocalClientConnectionState(0) != CA_ACTIVE || !clientUIActives[0].cgameInitialized || !KHZ_ClientOwnerCanAttemptStart()) return;
    const auto session = KHZ_AcceptedClientSession();
    if (!session || !KHZ_ClientInputFreezeBindings()) return;
    if (!KHZ_InputOwnerStart(reinterpret_cast<std::uintptr_t>(g_wv.hWnd))) Com_Error(ERR_DROP, "Could not create independent Raw Input owner");
    KHZ_InputOwnerSetGameplay(KHZ_ClientGameplayInputAllowed(), ++generation); KHZ_InputOwnerResume();
    realtimeOffset = Sys_Milliseconds() - cls.realtime;
    if (!KHZ_ClientOwnerStart(session, generation, {KHZ_ClientTransportPump, tick})) KHZ_InputOwnerPark();
}
