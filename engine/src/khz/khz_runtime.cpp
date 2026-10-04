#ifdef COD4X1KHZ
#include <khz/khz_render_profile.h>
#if defined(COD4X1KHZ_NATIVE_VULKAN) && !defined(KISAK_DEDICATED)
#include <gfx/legacy_factory.h>
#endif
#endif

#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include "khz/khz_archive_probe_boundary.h"
#endif
#include <universal/q_shared.h>
#include "khz_runtime.h"
#include "khz_benchmark_scene.h"
#include "khz_paths.h"
#include "khz_collision.h"
#include "khz_bots.h"
#include "khz_bot_inspection.h"
#include "khz_host.h"
#include "khz_profile.h"
#include "khz_profile_client.h"
#include "khz_ui.h"
#include "khz_admin_server.h"
#include "khz_plugins.h"
#include "khz_content_server.h"
#include "khz_services_client.h"
#include "khz_update_client.h"
#include "khz_input_owner.h"
#include "khz_client_owner.h"
#include "khz_client_transport.h"
#include "khz_controls.h"
#include "khz_content.h"
#include "khz_screenshot.h"
#include "khz_snapshot_headers.h"
#include "khz_server_metrics.h"
#include "khz_client_checkpoint.h"
#include "khz_server_capture.h"
#include <universal/com_files.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <qcommon/msg_mp.h>
#include <qcommon/qcommon.h>
#include <script/scr_vm.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_public_mp.h>
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <cgame_mp/cg_local_mp.h>
#include <win32/win_local.h>
#include <Windows.h>
#include <DbgHelp.h>
#include <database/database.h>
#include <array>
#include <cmath>
#include <memory>
#include <algorithm>
#include <publication.hpp>
#include <fp_environment.hpp>
#include <input_timestamp.hpp>

void KHZ_AdvanceGunRecoil(cg_s* game);

namespace {
struct QueuedCommand { usercmd_s command{}; std::uint64_t sequence = 0, viewTick = 0; };
struct RewindPose { float origin[3]{}, mins[3]{}, maxs[3]{}; int generation = 0, spawnTime = 0; bool playing = false; };
khz::TickHistory<std::array<RewindPose, 64>, khz::RewindHistory> rewindPoses;
struct Peer {
    std::array<QueuedCommand, khz::InputHistory> queue{};
    unsigned read = 0, write = 0;
    khz::SequenceWindow<khz::InputHistory> sequences;
    khz::InputLatch<usercmd_s> input;
    std::uint64_t received = 0, applied = 0, packetInput = 0, commandInput = 0;
    std::uint64_t packetViewTick = 0, viewTick = 0;
    std::uint64_t snapshotTick = 0, uniqueSnapshots = 0, uniqueCommands = 0, bytesSent = 0;
    std::uint64_t playingTicks = 0;
    std::unique_ptr<clientSnapshot_t[]> frames;
};
std::array<Peer, 64> peers;
std::array<usercmd_s, khz::InputHistory> commands{};
std::array<outPacket_t, khz::SnapshotHistory> packets{};
std::array<clSnapshot_t, khz::SnapshotHistory> snapshots{};
std::array<entityState_s, khz::ParsedEntities> entities{};
std::array<clientState_s, khz::ParsedClients> states{};
std::array<ClientArchiveData, khz::InputHistory> archive{};
khz::DeadlineCalendar<32768> scriptCalendar;
khz::DeadlineClock frameClock;
khz::PresentationClock presentationClock;
struct PredictedFrame { playerState_s player{}; std::uint64_t session = 0, tick = 0; };
khz::TriplePublication<PredictedFrame> predictedFrames;
thread_local bool presentationContext = false;
std::uint64_t presentationStateEdits = 0;
khz::Histogram frontendWork;
khz::Tick scriptNow = 0, serverTick = 0;
khz::WireHeader serverHeader{};
khz::WireHeader predictionHeader{};
std::uint64_t session = 0, frameStart = 0, tickStart = 0, statsStart = 0, receivedCommands = 0, sentSnapshots = 0;
khz::Histogram work, lateness;
std::uint64_t backlogMax = 0;
std::uint64_t rejectedTime = 0, duplicateCommands = 0, queueDrops = 0;
std::uint64_t predictionTicks = 0, commandsSent = 0, snapshotsReceived = 0, presentations = 0;
// Written by the renderer backend, sampled/reset by the frontend owner.
std::atomic<std::uint64_t> gpuPresents{0}, gpuPresentFailures{0};
struct TimingCounters {
    std::atomic<std::uint64_t> count{0}, total{0}, maximum{0};
};
// Cumulative QPC wall times; stage intervals can overlap across threads.
std::array<TimingCounters, static_cast<unsigned>(KHZ_Timing::Count)> timings;
constexpr const char* timingNames[] = { "frame_work", "prediction", "frontend", "backend", "swap_fence", "present", "renderer_sync" };
std::uint64_t rendererUnreadyTicks = 0, presentationCapTicks = 0;
std::uint64_t benchmarkTick = 0;
std::uint64_t predictionSteps = 0, predictionCorrections = 0;
std::uint64_t snapshotCollisionTicks = 0, legacyCollisionTicks = 0;
std::uint64_t rewindQueries = 0, rewindMatches = 0, snapshotBytes = 0;
double predictionCorrectionMax = 0;
int inputTime = 0;
std::atomic<bool> clientActive{false};
bool benchmarkJoined = false;
bool scriptFrameEnd = false;
struct MouseSample { int dx, dy; std::uint64_t timestamp; };
khz::SpscQueue<MouseSample, 4096> mouseQueue;
struct KeySample { unsigned time; int key, down; std::uint64_t timestamp; };
cmd_function_s serverCaptureCommand;
khz::SpscQueue<KeySample, 4096> keyQueue;
std::atomic<bool> keyOverflow{false};
std::uint64_t keyDrops = 0;
bool rawRegistered = false;
std::uint64_t mouseDrops = 0;
struct AccumulatedPhase { int* owner = nullptr; khz::FractionalPhase phase; };
// Each gameplay/presentation owner retains its own fractional accumulators.
// No changes to legacy field offsets or allocation during gameplay.
thread_local std::array<AccumulatedPhase, 512> idlePhases;
bool initialized = false;
cmd_function_s statsCommand{}, resetCommand{}, jsonCommand{}, dumpCommand{}, replayCommand{}, rewindCommand{}, stallCommand{}, physicsCommand{};
cmd_function_s contentReferenceCommand{}, contentReadCommand{};
std::uint64_t usec(std::uint64_t ticks) { return ticks * 1000000 / khz::frequency(); }
Peer& peer(client_t* client) { const auto index = client - svs.clients; iassert(index >= 0 && index < 64); return peers[index]; }
void resetStats() {
    KHZ_DvarScope synchronization;
    KHZ_BotResetWorkStats();
    KHZ_ServerMetricsReset();
    work = {}; lateness = {}; statsStart = khz::counter();
    frontendWork = {}; presentationStateEdits = 0;
    receivedCommands = sentSnapshots = snapshotBytes = rewindQueries = rewindMatches = 0;
    backlogMax = rejectedTime = duplicateCommands = queueDrops = 0;
    predictionTicks = commandsSent = snapshotsReceived = presentations = predictionSteps = predictionCorrections = 0;
    snapshotCollisionTicks = legacyCollisionTicks = 0;
    gpuPresents.store(0, std::memory_order_relaxed); gpuPresentFailures.store(0, std::memory_order_relaxed);
    for (auto& t : timings) {
        t.count.store(0, std::memory_order_relaxed); t.total.store(0, std::memory_order_relaxed);
        t.maximum.store(0, std::memory_order_relaxed);
    }
    rendererUnreadyTicks = presentationCapTicks = 0;
    predictionCorrectionMax = 0;
    for (auto& p : peers) p.uniqueCommands = p.uniqueSnapshots = p.bytesSent = p.playingTicks = 0;
}
void printStats() {
    KHZ_DvarScope synchronization;
    const double seconds = double(khz::counter() - statsStart) / khz::frequency();
    Com_Printf(CON_CHANNEL_DONT_FILTER,
        "KHZ_STATS {\"seconds\":%.6f,\"ticks\":%llu,\"hz\":%.3f,\"work_p99_us\":%u,\"work_max_us\":%llu,\"late_p99_us\":%u,\"commands\":%llu,\"snapshots\":%llu,\"backlog_max\":%llu,\"mouse_drops\":%llu}\n",
        seconds, work.count, seconds ? work.count / seconds : 0, work.percentile(99), work.maximum,
        lateness.percentile(99), receivedCommands, sentSnapshots, backlogMax, mouseDrops);
    Com_Printf(CON_CHANNEL_DONT_FILTER,
        "KHZ_DETAIL {\"prediction_ticks\":%llu,\"commands_sent\":%llu,\"snapshots_received\":%llu,\"presentations\":%llu,\"time_rejected\":%llu,\"duplicates\":%llu,\"queue_drops\":%llu,\"clock\":%llu,\"input_time\":%i,\"ack_input\":%llu}\n",
        predictionTicks, commandsSent, snapshotsReceived, presentations, rejectedTime, duplicateCommands, queueDrops, frameClock.tick(), inputTime, serverHeader.input);
}
void writeHeader(msg_t* message, const khz::WireHeader& header) { std::uint8_t bytes[khz::WireBytes]; khz::encode(bytes, header); MSG_WriteData(message, bytes, sizeof(bytes)); }
bool readHeader(msg_t* message, khz::WireHeader& header) {
    if (message->readcount < 0 || message->cursize - message->readcount < khz::WireBytes) return false;
    const bool result = khz::decode(message->data + message->readcount, khz::WireBytes, header);
    for (unsigned i = 0; i < khz::WireBytes; ++i) MSG_ReadByte(message);
    return result && !message->overflowed;
}
}

namespace {
LONG WINAPI reportCrash(EXCEPTION_POINTERS* exception) {
    FILE* output = std::fopen("khz_crash.txt", "w");
    if (!output) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(output, "Exception %08lx at %p\n", exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(process, "../bin/Release", TRUE);
    auto context = *exception->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC = {context.Eip, 0, AddrModeFlat};
    frame.AddrFrame = {context.Ebp, 0, AddrModeFlat};
    frame.AddrStack = {context.Esp, 0, AddrModeFlat};
    for (unsigned i = 0; i < 40 && frame.AddrPC.Offset; ++i) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 1024]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
        DWORD64 offset; IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD lineOffset;
        if (SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol)) {
            std::fprintf(output, "%s + %llu", symbol->Name, offset);
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineOffset, &line)) std::fprintf(output, " (%s:%lu)", line.FileName, line.LineNumber);
            std::fputc('\n', output);
        } else std::fprintf(output, "%llx\n", frame.AddrPC.Offset);
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, process, GetCurrentThread(), &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    }
    std::fclose(output); SymCleanup(process);
    return EXCEPTION_EXECUTE_HANDLER;
}
}
void KHZ_InstallCrashReporter() { SetUnhandledExceptionFilter(reportCrash); }
void KHZ_PrintStats() { printStats(); }
const char* KHZ_StatsJSON() {
    KHZ_DvarScope synchronization;
    static char result[32768];
    const double seconds = double(khz::counter() - statsStart) / khz::frequency();
    std::snprintf(result, sizeof(result),
        "KHZ_JSON {\"seconds\":%.6f,\"ticks\":%llu,\"work_p99_us\":%u,\"work_max_us\":%llu,\"commands_received\":%llu,\"snapshots_sent\":%llu,\"prediction_ticks\":%llu,\"commands_sent\":%llu,\"snapshots_received\":%llu,\"presentations\":%llu,\"time_rejected\":%llu,\"duplicates\":%llu,\"queue_drops\":%llu,\"clock\":%llu,\"debt\":%llu,\"backlog_max\":%llu,\"input_time\":%i,\"ack_input\":%llu,\"prediction_steps\":%llu,\"prediction_corrections\":%llu,\"prediction_correction_max\":%.6f,\"script_clock\":%llu,\"snapshot_bytes\":%llu,\"rewind_queries\":%llu,\"rewind_matches\":%llu,\"late_p99_us\":%u,\"late_max_us\":%llu,\"mouse_drops\":%llu,\"key_drops\":%llu}",
        seconds, work.count, work.percentile(99), work.maximum, receivedCommands, sentSnapshots,
        predictionTicks, commandsSent, snapshotsReceived, presentations, rejectedTime, duplicateCommands,
        queueDrops, frameClock.tick(), frameClock.debt(khz::counter()), backlogMax, inputTime, serverHeader.input,
        predictionSteps, predictionCorrections, predictionCorrectionMax, scriptNow, snapshotBytes, rewindQueries, rewindMatches,
        lateness.percentile(99), lateness.maximum, mouseDrops, keyDrops);
    auto used = std::strlen(result);
    result[--used] = 0; // append peer counters to the same JSON object
    used += std::snprintf(result + used, sizeof(result) - used, ",\"peers\":[");
    bool first = true;
    if (com_sv_running->current.enabled && svs.clients) {
        for (int i = 0; i < sv_maxclients->current.integer && used < sizeof(result) - 2048; ++i) {
            const auto* client = &svs.clients[i];
            if (client->header.state != CS_ACTIVE || KHZ_IsNativeBot(client)) continue;
            const auto& p = peers[i];
            const auto& player = level.clients[i];
            used += std::snprintf(result + used, sizeof(result) - used, "%s{\"client\":%i,\"port\":%u,\"commands\":%llu,\"snapshots\":%llu,\"bytes\":%llu,\"playing_ticks\":%llu,\"session_state\":%i,\"health\":%i}",
                first ? "" : ",", i, static_cast<unsigned short>(BigShort(client->header.netchan.remoteAddress.port)), p.uniqueCommands, p.uniqueSnapshots, p.bytesSent, p.playingTicks, player.sess.sessionState, g_entities[i].health);
            first = false;
        }
    }
    used += std::snprintf(result + used, sizeof(result) - used,
        "],\"gpu_presents\":%llu,\"gpu_present_failures\":%llu,\"process_id\":%lu,\"main_thread_id\":%lu,\"render_thread_id\":%lu,\"renderer_unready_ticks\":%llu,\"presentation_cap_ticks\":%llu,\"render_cap\":%i,\"frontend_p99_us\":%u,\"presentation_state_edits\":%llu,\"snapshot_collision_ticks\":%llu,\"legacy_collision_ticks\":%llu,\"timings\":{",
        gpuPresents.load(std::memory_order_relaxed), gpuPresentFailures.load(std::memory_order_relaxed),
        GetCurrentProcessId(), threadId[THREAD_CONTEXT_MAIN], threadId[THREAD_CONTEXT_BACKEND], rendererUnreadyTicks, presentationCapTicks,
        com_maxfps->current.integer, frontendWork.percentile(99), presentationStateEdits, snapshotCollisionTicks, legacyCollisionTicks);
    for (unsigned i = 0; i < timings.size(); ++i) {
        const auto& t = timings[i];
        used += std::snprintf(result + used, sizeof(result) - used,
            "%s\"%s\":{\"count\":%llu,\"total_us\":%llu,\"max_us\":%llu}", i ? "," : "", timingNames[i],
            t.count.load(std::memory_order_relaxed), usec(t.total.load(std::memory_order_relaxed)), usec(t.maximum.load(std::memory_order_relaxed)));
    }
    const auto inputOwner = KHZ_GetInputOwnerStats();
    used += std::snprintf(result + used, sizeof(result) - used,
        "},\"presentation_state\":{\"connection\":%d,\"screen_initialized\":%s,\"screen_recursion_guard\":%s,\"engine_error\":%s,\"renderer_started\":%s,\"window_minimized\":%s,\"width\":%u,\"height\":%u},\"raw_input\":{\"running\":%s,\"parked\":%s,\"focused\":%s,\"queued\":%u,\"dropped\":%llu,\"window_rebinds\":%llu,\"window_rebind_failures\":%llu,\"last_error\":%u}",
        CL_GetLocalClientConnectionState(0), scr_initialized ? "true" : "false",
        updateScreenCalled ? "true" : "false", com_errorEntered ? "true" : "false",
        cls.rendererStarted ? "true" : "false", g_wv.isMinimized ? "true" : "false", cls.vidConfig.displayWidth, cls.vidConfig.displayHeight,
        inputOwner.running ? "true" : "false", inputOwner.parked ? "true" : "false", inputOwner.focused ? "true" : "false",
        inputOwner.queued, inputOwner.dropped, inputOwner.windowRebinds, inputOwner.windowRebindFailures, inputOwner.lastError);
    std::snprintf(result + used, sizeof(result) - used, ",\"server_phases\":%s,\"native_bots\":%s,\"host\":%s,\"ui\":%s,\"client_owner\":%s,\"plugins\":%s,\"content_server\":%s,\"services_client\":%s,\"updates\":%s,\"screenshot_client\":%s,\"screenshot_server\":%s}",
        KHZ_ServerMetricsJSON(), KHZ_BotStatsJSON(), KHZ_HostStatusJSON(), KHZ_UIStatusJSON(), KHZ_ClientOwnerStatusJSON(), KHZ_PluginStatsJSON(),
        KHZ_ContentServerStatsJSON(), KHZ_ServicesClientStatusJSON(), KHZ_UpdateClientStatusJSON(),
        KHZ_ScreenshotClientStatsJSON(), KHZ_ScreenshotServerStatsJSON());
#if defined(COD4X1KHZ_NATIVE_VULKAN) && !defined(KISAK_DEDICATED)
    used=std::strlen(result);
    if(used && used+1024<sizeof(result))std::snprintf(result+used-1,sizeof(result)-used+1,",\"native_graphics\":%s}",gfx::legacy::frameTelemetry());
#endif
    return result;
}
const char* KHZ_LogName() {
    static char name[64];
    if (KHZ_IsHeadless()) std::snprintf(name, sizeof(name), "khz_bench_%lu.log", GetCurrentProcessId());
    else std::strcpy(name, com_dedicated && com_dedicated->current.integer ? "khz_server.log" : "khz_client.log");
    return name;
}
bool KHZ_IsHeadless() {
    static const bool result = [] { char path[MAX_PATH]{}; GetModuleFileNameA(nullptr, path, sizeof(path)); return std::strstr(path, "_bench.exe") != nullptr; }();
    return result;
}
int KHZ_InputTime() {
    return ++inputTime;
}
void KHZ_CommitSnapshot(int messageNumber) {
    // Only a successfully decoded snapshot can advance prediction's baseline.
    const auto* cl = CL_GetLocalClientGlobals(0);
    if (!cl->snap.valid || cl->snap.messageNum != messageNumber) return;
    if (!KHZ_RecordCommittedSnapshotHeader(messageNumber, serverHeader)) return;
    predictionHeader = serverHeader;
    // Asset/audio reload can pause command production while the dedicated
    // server continues advancing native commandTime. Retaining only ++inputTime
    // makes fresh commands fail the server's two-second admission window, then
    // stalls acknowledgements and amplifies prediction replay. Rebase the
    // admission timestamp from this accepted player state, without changing
    // command sequences, saved inputs, acknowledgements or one-ms Pmove steps.
    inputTime = khz::alignInputTimestamp(inputTime, cl->snap.ps.commandTime);
    ++snapshotsReceived;
}
std::uint64_t KHZ_PredictionAck() { return predictionHeader.input; }
std::uint64_t KHZ_AcceptedClientSession() { return serverHeader.session; }
#ifndef KISAK_DEDICATED
namespace {
struct RuntimeCheckpointImage {
    decltype(commands) commands;
    decltype(packets) packets;
    decltype(snapshots) snapshots;
    decltype(entities) entities;
    decltype(states) states;
    decltype(archive) archive;
    decltype(idlePhases) phases;
    khz::SnapshotHeaderHistory committedHeaders;
    khz::WireHeader serverHeader, predictionHeader;
    khz::checkpoint::Frame frame;
    std::uint64_t benchmarkTick;
    int inputTime;
    bool clientActive, benchmarkJoined, captured;
};
RuntimeCheckpointImage runtimeImage; // fixture-only BSS, never a tick allocation
bool runtimeReject(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text);
    return false;
}
bool runtimeQuiescent(const khz::checkpoint::Frame& frame, char* reason, unsigned capacity) {
    if (!KHZ_IsClientSimulationThread() || !KHZ_ClientOwnerCheckpointContext())
        return runtimeReject(reason, capacity, "runtime_checkpoint_requires_parked_worker");
    if (!KHZ_ClientTransportCheckpointFenced(frame.fixture, frame.session, frame.generation))
        return runtimeReject(reason, capacity, "runtime_checkpoint_requires_verified_transport_fence");
    if (!clientActive.load(std::memory_order_acquire) || serverHeader.session != frame.session ||
        predictionHeader.session != frame.session || inputTime <= 0)
        return runtimeReject(reason, capacity, "runtime_checkpoint_has_no_active_acknowledged_session");
    if (mouseQueue.size() || keyQueue.size() || keyOverflow.load(std::memory_order_acquire))
        return runtimeReject(reason, capacity, "runtime_checkpoint_legacy_input_queue_not_empty");
    const auto& client = *CL_GetLocalClientGlobals(0);
    if (client.cmdNumber < 1 || client.snap.messageNum < 1 ||
        client.parseEntitiesNum < 0 || client.parseClientsNum < 0)
        return runtimeReject(reason, capacity, "runtime_checkpoint_native_ring_cursor_invalid");
    khz::WireHeader accepted;
    if (!KHZ_CommittedSnapshotHeader(client.snap.messageNum, &accepted) || accepted.session != frame.session)
        return runtimeReject(reason, capacity, "runtime_checkpoint_committed_header_missing");
    return true;
}
bool runtimeCapture(const khz::checkpoint::Frame& frame, char* reason, unsigned capacity) {
    if (!runtimeQuiescent(frame, reason, capacity)) return false;
    runtimeImage.commands = commands; runtimeImage.packets = packets;
    runtimeImage.snapshots = snapshots; runtimeImage.entities = entities;
    runtimeImage.states = states; runtimeImage.archive = archive;
    runtimeImage.phases = idlePhases;
    KHZ_CopyCommittedSnapshotHeaders(&runtimeImage.committedHeaders);
    runtimeImage.serverHeader = serverHeader; runtimeImage.predictionHeader = predictionHeader;
    runtimeImage.inputTime = inputTime; runtimeImage.benchmarkTick = benchmarkTick;
    runtimeImage.benchmarkJoined = benchmarkJoined;
    runtimeImage.clientActive = clientActive.load(std::memory_order_acquire);
    runtimeImage.frame = frame; runtimeImage.captured = true;
    return true;
}
bool runtimeValidate(const khz::checkpoint::Frame& frame, char* reason, unsigned capacity) {
    if (!runtimeQuiescent(frame, reason, capacity)) return false;
    return runtimeImage.captured && khz::checkpoint::sameLifetime(runtimeImage.frame, frame) ? true :
        runtimeReject(reason, capacity, "runtime_checkpoint_image_lifetime_changed");
}
bool runtimeRestore(const khz::checkpoint::Frame& frame, std::uint64_t, char* reason, unsigned capacity) {
    // The coordinator validated every provider before any restore write. The
    // native client provider has already restored its corresponding cursors.
    if (!runtimeImage.captured || !khz::checkpoint::sameLifetime(runtimeImage.frame, frame) ||
        !KHZ_IsClientSimulationThread() || !KHZ_ClientOwnerCheckpointContext() ||
        !KHZ_ClientTransportCheckpointFenced(frame.fixture, frame.session, frame.generation))
        return runtimeReject(reason, capacity, "runtime_checkpoint_restore_fence_or_lifetime_changed");
    commands = runtimeImage.commands; packets = runtimeImage.packets;
    snapshots = runtimeImage.snapshots; entities = runtimeImage.entities;
    states = runtimeImage.states; archive = runtimeImage.archive;
    idlePhases = runtimeImage.phases;
    KHZ_RestoreCommittedSnapshotHeaders(runtimeImage.committedHeaders);
    serverHeader = runtimeImage.serverHeader; predictionHeader = runtimeImage.predictionHeader;
    inputTime = runtimeImage.inputTime; benchmarkTick = runtimeImage.benchmarkTick;
    benchmarkJoined = runtimeImage.benchmarkJoined;
    clientActive.store(runtimeImage.clientActive, std::memory_order_release);
    return true;
}
void runtimeDiscard() { runtimeImage.captured = false; }
}
#endif
void KHZ_ClientRuntimeRegisterCheckpoint() {
#ifndef KISAK_DEDICATED
    iassert(Sys_IsMainThread());
    const KHZ_ClientCheckpointProvider provider{khz::checkpoint::Domain::RuntimeHistory, sizeof(runtimeImage),
        runtimeCapture, runtimeValidate, runtimeRestore, runtimeDiscard};
    if (!KHZ_ClientCheckpointRegister(provider)) Com_Error(ERR_FATAL, "Duplicate runtime checkpoint provider");
#endif
}
void KHZ_ResetClientTransport() {
    serverHeader = {}; predictionHeader = {}; clientActive = false; inputTime = 0;
    KHZ_ResetCommittedSnapshotHeaders(0);
    KHZ_UnbindPredictionCollisionFrame();
}
void KHZ_ClientPresentationRestarted() {
    iassert(Sys_IsMainThread());
    iassert(!KHZ_ClientOwnerRunning());
    clientActive.store(false, std::memory_order_release);
    frameClock.reset(khz::counter(), khz::frequency());
    frameStart = frameClock.deadline();
    presentationClock = {};
}
void KHZ_RecordPredictionCorrection(float distance) { ++predictionCorrections; if (distance > predictionCorrectionMax) predictionCorrectionMax = distance; }
void KHZ_RecordPredictionStep() { ++predictionSteps; }
int KHZ_PredictionFirstCommand(int current) {
    const int oldest = (std::max)(1, current - int(khz::InputHistory - 1));
    if (predictionHeader.input >= static_cast<std::uint64_t>(current)) return current + 1;
    return (std::max)(oldest, static_cast<int>(predictionHeader.input + 1));
}
const char* KHZ_StateJSON() {
    KHZ_DvarScope synchronization;
    static char result[4096];
    const auto* cl = CL_GetLocalClientGlobals(0);
    const auto& ps = cl->snap.ps;
    const auto* game = CG_GetLocalClientGlobals(0);
    char map[sizeof(cl->mapname)]{};
    for (unsigned i = 0; i + 1 < sizeof(map) && cl->mapname[i]; ++i) {
        const auto c = static_cast<unsigned char>(cl->mapname[i]);
        map[i] = c < 32 || c == '"' || c == '\\' ? '_' : static_cast<char>(c);
    }
    int used = std::snprintf(result, sizeof(result), "KHZ_STATE {\"map\":\"%s\",\"connection\":%i,\"server_time\":%i,\"command_time\":%i,\"pm_type\":%i,\"other_flags\":%i,\"view_client\":%i,\"weapon\":%u,\"server_id\":%i,\"benchmark\":%i,\"benchmark_tick\":%llu,\"origin\":[%.4f,%.4f,%.4f],\"menus\":[", map, CL_GetLocalClientConnectionState(0), cl->snap.serverTime, ps.commandTime, ps.pm_type, ps.otherFlags, ps.clientNum, ps.weapon, cl->serverId, Dvar_GetBool("khz_benchmark"), benchmarkTick, ps.origin[0], ps.origin[1], ps.origin[2]);
    bool first = true;
    for (int i = 0; i < 32 && used < int(sizeof(result) - 512); ++i) {
        const auto* menu = CL_GetConfigString(0, CS_SCRIPT_MENUS + i);
        if (!menu || !*menu) continue;
        used += std::snprintf(result + used, sizeof(result) - used, "%s[%i,\"%s\"]", first ? "" : ",", i, menu);
        first = false;
    }
    std::snprintf(result + used, sizeof(result) - used,
        "],\"fov_scale\":%.6f,\"weapon_pos_frac\":%.6f,\"predicted_weapon_pos_frac\":%.6f,\"tan_half_fov_x\":%.6f,\"tan_half_fov_y\":%.6f,\"zoom_sensitivity\":%.6f,\"rendered_view_origin\":[%.4f,%.4f,%.4f],\"rendered_view_angles\":[%.4f,%.4f,%.4f]}",
        Dvar_GetFloat("cg_fovScale"), ps.fWeaponPosFrac, game->predictedPlayerState.fWeaponPosFrac,
        game->refdef.tanHalfFovX, game->refdef.tanHalfFovY, game->zoomSensitivity,
        game->refdef.vieworg[0], game->refdef.vieworg[1], game->refdef.vieworg[2],
        game->refdefViewAngles[0], game->refdefViewAngles[1], game->refdefViewAngles[2]); return result;
}
void KHZ_BenchmarkInput(usercmd_s* command) {
    if (!Dvar_GetBool("khz_benchmark")) return;
    if (KHZ_BenchmarkSceneInput(command)) return;
    const auto phase = unsigned(benchmarkTick % 10000);
    command->angles[0] = 0;
    command->angles[1] = static_cast<unsigned short>((benchmarkTick / 2500 % 4) * 16384);
    command->angles[2] = 0;
    command->forwardmove = phase < 2500 ? 127 : 0;
    command->rightmove = phase >= 2500 && phase < 4500 ? 127 : 0;
    command->buttons = 0;
    if (phase >= 4500 && phase < 4510) command->buttons |= BUTTON_JUMP;
    if (phase >= 5000 && phase < 6500) command->buttons |= BUTTON_ATTACK;
    if (phase >= 6600 && phase < 6610) command->buttons |= BUTTON_RELOAD;
    if (phase >= 7500 && phase < 7510) command->buttons |= BUTTON_FRAG;
    if (Dvar_GetBool("khz_test_controls") && Dvar_GetBool("khz_test_ads")) command->buttons |= BUTTON_ADS;
    const auto& ps = CL_GetLocalClientGlobals(0)->snap.ps;
    if ((ps.otherFlags & 2) || ps.pm_type >= PM_DEAD) {
        // Use the stock respawn/skip-killcam input. Do not change teams or
        // classes while a normal death/respawn thread owns the player.
        command->forwardmove = command->rightmove = 0;
        command->buttons = benchmarkTick % 500 < 50 ? BUTTON_USE : 0;
    }
}

const char* KHZ_ContentFixtureReadJSON(const char* path) {
    // Diagnostic lifecycle work uses the real mounted filesystem. Reading the
    // marker also marks its containing IWD referenced for native pure lists.
    iassert(Sys_IsMainThread());
    static char result[512];
    bool safe = path && !std::strncmp(path, "khz_fixture_", 12) && std::strlen(path) < 128;
    if (safe) for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) safe = false;
    bool ok = false; unsigned bytes = 0, crc = 0;
    if (safe && Dvar_GetBool("khz_test_controls")) {
        int handle = 0;
        const auto length = FS_FOpenFileRead(path, &handle);
        if (handle) {
            unsigned char data[4096];
            if (length && length <= sizeof(data) && FS_Read(data, length, handle) == length) {
                bytes = length; crc = khz::content::Crc32(data, bytes); ok = true;
            }
            FS_FCloseFile(handle);
        }
    }
    std::snprintf(result, sizeof(result), "{\"ok\":%s,\"path\":\"%s\",\"bytes\":%u,\"crc\":%u}", ok ? "true" : "false", safe ? path : "", bytes, crc);
    return result;
}
void KHZ_Init() {
    if (initialized) return;
    initialized = true;
#ifdef DEDICATED
    khz::configureGameplayFpEnvironment();
#endif
    KHZ_InitHost();
    KHZ_BotInit();
#ifdef DEDICATED
    KHZ_ScreenshotServerInit();
#endif
#ifndef DEDICATED
    KHZ_InitClientCheckpoint();
    KHZ_ClientRuntimeRegisterCheckpoint();
    KHZ_ClientTransportRegisterCheckpoint();
    KHZ_InitProfileFeatures();
    KHZ_InitProfileClientFeatures();
    KHZ_InitServicesClientFeatures();
    KHZ_InitUpdateClientFeatures();
    KHZ_ScreenshotClientInit();
#endif
    KHZ_InitCollisionDiagnostics();
    Cmd_AddCommandInternal("khz_stats", printStats, &statsCommand);
    const auto readContentFixture = [] {
        const auto* report = KHZ_ContentFixtureReadJSON(Cmd_Argc() == 2 ? Cmd_Argv(1) : nullptr);
        if (Cmd_Argc() == 2 && !I_stricmp(Cmd_Argv(0), "khz_content_fixture_reference") &&
            Dvar_GetBool("khz_test_controls") && Dvar_GetBool("khz_benchmark") &&
            com_sv_running->current.enabled && std::strstr(report, "\"ok\":true")) {
            Dvar_SetStringByName("sv_referencedIwds", FS_ReferencedIwdChecksums());
            Dvar_SetStringByName("sv_referencedIwdNames", FS_ReferencedIwdNames());
            SV_SaveSystemInfo();
        }
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_CONTENT_FIXTURE %s\n", report);
    };
    Cmd_AddCommandInternal("khz_content_fixture_reference", readContentFixture, &contentReferenceCommand);
    Cmd_AddCommandInternal("khz_content_fixture_read", readContentFixture, &contentReadCommand);
    Cmd_AddCommandInternal("khz_resetstats", resetStats, &resetCommand);
    Cmd_AddCommandInternal("khz_server_capture", [] {
        static std::uint64_t fixture = 0;
        const char* action = Cmd_Argc() > 1 ? Cmd_Argv(1) : "status";
        if (!Dvar_GetBool("khz_benchmark") || !com_sv_running->current.enabled) {
            Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_SERVER_CAPTURE {\"result\":\"requires an isolated native server\"}\n"); return;
        }
        if (!I_stricmp(action, "begin") && Cmd_Argc() == 3) {
            char* end = nullptr; const auto count = std::strtoul(Cmd_Argv(2), &end, 10);
            if (*Cmd_Argv(2) && end && !*end && count && count <= khz::server_capture::MaxTicks) {
                if (!++fixture) ++fixture;
                KHZ_ServerCaptureBegin(fixture, static_cast<unsigned>(count));
            }
        } else if (!I_stricmp(action, "end")) KHZ_ServerCaptureEnd();
        else if (!I_stricmp(action, "export") && Cmd_Argc() == 2) {
            Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "KHZ_SERVER_CAPTURE_EXPORT ", 0);
            Com_PrintMessage(CON_CHANNEL_DONT_FILTER, KHZ_ServerCaptureExportJSON(), 0);
            Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "\n", 0); return;
        }
        else if (!I_stricmp(action, "drain")) {
            static khz::server_capture::TickRecord record;
            while (KHZ_ServerCapturePop(&record)) {}
        } else if (!I_stricmp(action, "read") && Cmd_Argc() == 3) {
            char* end = nullptr; const auto ordinal = std::strtoul(Cmd_Argv(2), &end, 10);
            if (*Cmd_Argv(2) && end && !*end && ordinal && ordinal <= khz::server_capture::MaxTicks) {
                Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "KHZ_SERVER_CAPTURE_TICK ", 0);
                Com_PrintMessage(CON_CHANNEL_DONT_FILTER, KHZ_ServerCaptureTickJSON(static_cast<unsigned>(ordinal)), 0);
                Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "\n", 0); return;
            }
        }
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "KHZ_SERVER_CAPTURE ", 0);
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, KHZ_ServerCaptureStatusJSON(), 0);
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "\n", 0);
    }, &serverCaptureCommand);
    Cmd_AddCommandInternal("khz_json", [] {
        // Avoid the legacy formatted-print 4 KB limit for larger peer reports.
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, KHZ_StatsJSON(), 0);
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "\n", 0);
    }, &jsonCommand);
#if defined(KISAK_DEDICATED)
    static cmd_function_s botInspectionCommand{};
    Cmd_AddCommandInternal("khz_bot_inspect", [] {
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "KHZ_BOT_INSPECTION ", 0);
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, KHZ_BotInspectionJSON(), 0);
        Com_PrintMessage(CON_CHANNEL_DONT_FILTER, "\n", 0);
    }, &botInspectionCommand);
#endif
    Cmd_AddCommandInternal("khz_replay", KHZ_RunMovementReplay, &replayCommand);
    Cmd_AddCommandInternal("khz_rewind_test", KHZ_RunRewindProbe, &rewindCommand);
    Cmd_AddCommandInternal("khz_physics_test", KHZ_RunPhysicsProbe, &physicsCommand);
    Cmd_AddCommandInternal("khz_stall", [] {
        if (!Dvar_GetBool("khz_test_controls")) {
            Com_Printf(CON_CHANNEL_DONT_FILTER, "khz_stall requires +set khz_test_controls 1 at startup\n"); return;
        }
        const int milliseconds = Cmd_Argc() == 2 ? std::atoi(Cmd_Argv(1)) : 0;
        if (milliseconds < 1 || milliseconds > 2000) {
            Com_Printf(CON_CHANNEL_DONT_FILTER, "khz_stall accepts 1 through 2000 milliseconds\n"); return;
        }
        // A diagnostic command, outside G_RunFrame; retained debt must recover.
        Sleep(milliseconds);
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_STALL {\"milliseconds\":%i,\"debt\":%llu}\n", milliseconds, frameClock.debt(khz::counter()));
    }, &stallCommand);
    Cmd_AddCommandInternal("khz_dump_scripts", [] {
        for (const auto* name : {"_callbacksetup", "_globallogic", "_teams", "_class", "_menus", "_rank", "_weapons"}) {
            auto* raw = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, va("maps/mp/gametypes/%s.gsc", name)).rawfile;
            if (!raw || !raw->buffer) continue;
            auto* file = std::fopen(va("khz_test%s.gsc", name), "wb");
            if (!file) continue;
            std::fwrite(raw->buffer, 1, std::strlen(raw->buffer), file); std::fclose(file);
        }
    }, &dumpCommand);
    Dvar_RegisterInt("khz_tickrate", 1000, 1000, 1000, DVAR_ROM | DVAR_SERVERINFO, "Fixed gameplay ticks per second");
    // The stock registry replaces startup user-created INIT values with its
    // supplied default. Capture them before registration so explicit startup
    // choices survive, while later console writes remain protected.
    const bool benchmarkStartup = KHZ_IsHeadless() || KHZ_IsBenchmarkHome() || Dvar_GetBool("khz_benchmark");
    const bool controlsStartup = Dvar_GetBool("khz_test_controls");
    const bool scriptProfileStartup = benchmarkStartup && Dvar_GetBool("khz_profile_scripts");
    const auto* benchmarkVar = Dvar_RegisterBool("khz_benchmark", benchmarkStartup, DVAR_INIT, "Use repeatable test inputs and join automatically");
    const auto* controlsVar = Dvar_RegisterBool("khz_test_controls", controlsStartup, DVAR_INIT, "Enable bounded diagnostic scheduler stalls");
    Dvar_SetBool(benchmarkVar, benchmarkStartup);
    Dvar_SetBool(controlsVar, controlsStartup);
    const auto* scriptProfileVar = Dvar_RegisterBool("khz_profile_scripts", scriptProfileStartup, DVAR_INIT, "Measure native script slices in an isolated benchmark");
    Dvar_SetBool(scriptProfileVar, scriptProfileStartup);
    Dvar_RegisterBool("khz_test_ads", false, 0, "Isolated benchmark ADS input for native FOV checks");
    Dvar_RegisterBool("khz_snapshot_collision", true, DVAR_INIT, "Predict against fixed snapshot collision frames when complete; count legacy fallback otherwise");
    resetStats();
    KHZ_RenderProfileInit();
    KHZ_BenchmarkSceneInit();
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    if (!KHZ_InitArchiveCodecProbeCommand())
        Com_Error(ERR_FATAL, "Could not install authenticated archive codec probe");
#endif
}
void KHZ_WaitFrame() {
    // Com_Error uses longjmp; clear a scope abandoned by a render error.
    presentationContext = false;
    KHZ_UnbindPredictionCollisionFrame();
    KHZ_Init();
    if (!frameClock.frequency()) frameClock.reset(khz::counter(), khz::frequency());
    frameStart = frameClock.deadline();
#ifndef KISAK_DEDICATED
    if (KHZ_ClientOwnerRunning() && frameClock.debt(khz::counter()) > 1) {
        // The frontend has no gameplay debt: its independent owner retains
        // every simulation tick while presentation skips superseded frames.
        frameClock.reset(khz::counter(), khz::frequency());
        frameStart = frameClock.deadline();
    }
#endif
#ifndef KISAK_DEDICATED
    // A single mutable frontend owner services two independent clocks. Extra
    // presentations do not run packet handlers, commands, input, or prediction.
    // A separate simulation owner still requires extracting the legacy CG world.
    if (clientActive && !KHZ_IsHeadless() && !com_dedicated->current.integer) {
        const auto f = khz::frequency();
        for (;;) {
            const auto now = khz::counter();
            if (now >= frameStart) break;
            const auto fps = static_cast<unsigned>((std::max)(0, com_maxfps->current.integer));
            if (!presentationClock.frequency() || presentationClock.rate() != fps)
                presentationClock.reset(now, f, fps);
            if (presentationClock.deadline() > now) {
                const auto wake = (std::min)(frameStart, presentationClock.deadline());
                khz::waitUntil(wake);
                continue;
            }
            if (Sys_IsRendererReady()) {
                KHZ_PresentFrame();
            } else {
                khz::waitUntilOrEvent(frameStart, Sys_GetRendererCompletionEvent());
            }
        }
    }
#endif
    khz::waitUntil(frameStart);
    const auto now = khz::counter();
    const auto debt = frameClock.debt(now); if (debt > backlogMax) backlogMax = debt;
    lateness.add(now > frameStart ? usec(now - frameStart) : 0);
    frameClock.advance();
}
bool KHZ_PresentDue() {
    if (KHZ_IsHeadless() && CL_GetLocalClientConnectionState(0) == CA_ACTIVE) return false;
    const auto fps = static_cast<unsigned>((std::max)(0, com_maxfps->current.integer));
    if (!presentationClock.frequency() || presentationClock.rate() != fps)
        presentationClock.reset(frameStart, khz::frequency(), fps);
    const bool ready = Sys_IsRendererReady() != 0;
    if (!ready) ++rendererUnreadyTicks;
    if (!presentationClock.present(khz::counter(), ready)) {
        if (ready) ++presentationCapTicks;
        return false;
    }
    ++presentations; return true;
}
void KHZ_PresentFrame() {
#ifndef KISAK_DEDICATED
    if (clientActive && !KHZ_ClientOwnerRunning() && frameClock.debt(khz::counter())) return;
    if (!KHZ_PresentDue()) return;
    const auto started = khz::counter();
    if (KHZ_ClientOwnerRunning()) {
        KHZ_RenderProfileScope consumeTrace(KHZ_RenderStage::Consume);
        KHZ_ClientOwnerConsumePresentation();
    }
    {
        KHZ_RenderProfileScope frontendTrace(KHZ_RenderStage::Frontend);
        KHZ_TimingScope frontendTimer(KHZ_Timing::Frontend);
        // Stage the published player view for legacy camera functions. Their
        // vehicle-seat projections must never feed back into gameplay state.
        auto* cg = CG_GetLocalClientGlobals(0);
        const auto& published = predictedFrames.readLatest();
        const bool usePublished = !KHZ_ClientOwnerRunning() && clientActive && published.session == serverHeader.session && published.tick;
        const auto savedPlayer = cg->predictedPlayerState;
        const int savedTime = cg->time, savedOldTime = cg->oldTime, savedFrameTime = cg->frametime;
        const int savedBgTime = cg->bgs.time, savedBgFrameTime = cg->bgs.frametime;
        if (usePublished) cg->predictedPlayerState = published.player;
        struct Context {
            bool previous = presentationContext;
            Context() { presentationContext = true; }
            ~Context() { presentationContext = previous; }
        } context;
        SCR_UpdateScreen();
        if (usePublished) {
            if (std::memcmp(&cg->predictedPlayerState, &published.player, sizeof(playerState_s))) ++presentationStateEdits;
            cg->predictedPlayerState = savedPlayer;
            cg->time = savedTime; cg->oldTime = savedOldTime; cg->frametime = savedFrameTime;
            cg->bgs.time = savedBgTime; cg->bgs.frametime = savedBgFrameTime;
        }
    }
    if (clientActive) {
        const auto elapsed = khz::counter() - started;
        frontendWork.add(usec(elapsed));
    }
#endif
}
bool KHZ_IsPresentationContext() { return presentationContext; }
void KHZ_RecordGpuPresent(bool succeeded) {
    (succeeded ? gpuPresents : gpuPresentFailures).fetch_add(1, std::memory_order_relaxed);
}
void KHZ_RecordTiming(KHZ_Timing stage, std::uint64_t elapsed) {
    auto& t = timings[static_cast<unsigned>(stage)];
    t.total.fetch_add(elapsed, std::memory_order_relaxed);
    auto maximum = t.maximum.load(std::memory_order_relaxed);
    while (maximum < elapsed && !t.maximum.compare_exchange_weak(maximum, elapsed, std::memory_order_relaxed)) {}
    t.count.fetch_add(1, std::memory_order_relaxed);
}
void KHZ_ClientTick() {
#ifndef KISAK_DEDICATED
    BG_ClearPredictionAnimationOwner();
    KHZ_UnbindPredictionCollisionFrame();
    if (CL_GetLocalClientConnectionState(0) != CA_ACTIVE) { clientActive = false; return; }
    auto* cg = CG_GetLocalClientGlobals(0);
    if (cg->isLoading) return;
    KHZ_TimingScope predictionTimer(KHZ_Timing::Prediction);
    if (!clientActive) {
        // Asset loading is outside the active match clock. Never manufacture
        // hundreds of future inputs to repay pre-connection loading time.
        if (!KHZ_IsClientSimulationThread()) {
            frameClock.reset(khz::counter(), khz::frequency());
            frameStart = khz::counter();
            presentationClock = {};
            lateness = {}; backlogMax = 0;
        }
        inputTime = khz::alignInputTimestamp(inputTime, CL_GetLocalClientGlobals(0)->snap.ps.commandTime);
        idlePhases.fill(AccumulatedPhase{});
        clientActive = true;
        benchmarkTick = 0;
        benchmarkJoined = false;
    }
    cg->oldTime = cg->time;
    cg->time = CL_GetLocalClientGlobals(0)->serverTime;
    cg->frametime = 1; cg->bgs.frametime = 1; cg->bgs.time = cg->time;
    iassert(bgs == nullptr); bgs = &cg->bgs;
    if (KHZ_IsClientSimulationThread()) KHZ_ClientOwnerProcessSnapshots();
    else CG_ProcessSnapshots(0);
    if (KHZ_IsClientSimulationThread() && KHZ_ClientOwnerNeedsMainThread()) { bgs = nullptr; return; }
    if (cg->snap && cg->nextSnap) {
        const bool fixedCollision = KHZ_IsClientSimulationThread() ? KHZ_ClientOwnerBeginCollisionFrame() :
            Dvar_GetBool("khz_snapshot_collision") && KHZ_BeginClientCollisionFrame(serverHeader.session, frameClock.tick(), cg->time, cg->snap, cg->nextSnap);
        if (KHZ_IsClientSimulationThread() && !fixedCollision) { bgs = nullptr; return; }
        if (KHZ_IsClientSimulationThread()) {
            KHZ_ClientOwnerReadAimTargets();
            if (KHZ_ClientOwnerNeedsMainThread()) {
                KHZ_UnbindPredictionCollisionFrame(); bgs = nullptr; return;
            }
        }
        ++predictionTicks; ++benchmarkTick;
        if (fixedCollision) ++snapshotCollisionTicks;
        else ++legacyCollisionTicks;
        CG_UpdateShellShockInput(0);
        CG_UpdateCommandFeedback(0);
        CL_Input(0);
        CG_PredictPlayerState(0);
        KHZ_UnbindPredictionCollisionFrame();
        BG_UpdatePredictionAnimationState(&cg->predictedPlayerState, &cg->predictedPlayerEntity.nextState);
        if (KHZ_IsClientSimulationThread()) KHZ_AdvanceGunRecoil(cg);
        // Recoil and gameplay key-catchers advance on simulation ticks only.
        if (cg->nextSnap->ps.otherFlags & 4) CG_KickAngles(cg);
        else { std::fill_n(cg->kickAVel, 3, 0.0f); std::fill_n(cg->kickAngles, 3, 0.0f); }
        if (!cg->predictedPlayerState.locationSelectionInfo || (cg->predictedPlayerState.otherFlags & 2)) {
            if (KHZ_IsClientSimulationThread()) KHZ_ClientOwnerSetLocationSelection(false);
            else if (Key_IsCatcherActive(0, KEYCATCH_LOCATION_SELECTION)) Key_RemoveCatcher(0, ~KEYCATCH_LOCATION_SELECTION);
            cg->selectedLocation[0] = cg->selectedLocation[1] = .5f;
        } else if (!Key_IsCatcherActive(0, KEYCATCH_LOCATION_SELECTION)) {
            if (KHZ_IsClientSimulationThread()) KHZ_ClientOwnerSetLocationSelection(true);
            else Key_AddCatcher(0, KEYCATCH_LOCATION_SELECTION);
            cg->selectedLocation[0] = cg->selectedLocation[1] = .5f;
        }
        if (KHZ_IsClientSimulationThread()) KHZ_ClientOwnerPublishPrediction();
        else if (!KHZ_IsHeadless()) {
            auto& frame = predictedFrames.writable();
            frame.player = cg->predictedPlayerState;
            frame.session = serverHeader.session; frame.tick = frameClock.tick();
            predictedFrames.publish();
        }
    }
    bgs = nullptr;
    if (cg->nextSnap && cg->nextSnap->ps.pm_type == PM_NORMAL && !(cg->nextSnap->ps.otherFlags & 2) && cg->nextSnap->ps.weapon)
        benchmarkJoined = true;
    if (Dvar_GetBool("khz_benchmark") && !benchmarkJoined && cg->nextSnap
        && (cg->nextSnap->ps.pm_type == PM_SPECTATOR || (cg->nextSnap->ps.otherFlags & 2)) && benchmarkTick % 500 == 1) {
        const bool selectClass = (benchmarkTick / 500) & 1;
        for (int i = 0; i < 32; ++i) {
            const auto* menu = CL_GetConfigString(0, CS_SCRIPT_MENUS + i);
            const bool match = selectClass ? !std::strcmp(menu, "changeclass") : !std::strcmp(menu, "team_marinesopfor");
            if (match) {
                CL_AddReliableCommand(0, va("mr %i %i %s", CL_GetLocalClientGlobals(0)->serverId, i, selectClass ? "assault_mp" : "autoassign"));
                break;
            }
        }
    }
#endif
}
void KHZ_ResetServer() {
    KHZ_ServerCaptureInvalidateLifecycle();
    KHZ_BotTick(0);
    session = khz::counter() ^ (std::uint64_t(GetCurrentProcessId()) << 32);
    if (!session) session = 1;
    serverTick = 0;
    rewindPoses.reset();
    idlePhases.fill(AccumulatedPhase{});
    frameClock.reset(khz::counter(), khz::frequency());
    for (auto& p : peers) {
        p.read = p.write = 0; p.received = p.applied = p.packetInput = p.commandInput = 0;
        p.packetViewTick = p.viewTick = p.snapshotTick = 0;
        p.input.reset(); p.sequences.reset();
        if (p.frames) std::fill_n(p.frames.get(), khz::SnapshotHistory, clientSnapshot_t{});
    }
    resetStats();
}
void KHZ_ResetClient(client_t* client) {
    KHZ_BotResetSlot(client);
    auto& p = peer(client); p.read = p.write = 0; p.received = p.applied = p.packetInput = p.commandInput = 0; p.input.reset(); p.sequences.reset();
    p.packetViewTick = p.viewTick = serverTick;
    p.snapshotTick = p.uniqueSnapshots = p.uniqueCommands = p.bytesSent = p.playingTicks = 0;
    if (KHZ_IsNativeBot(client)) { p.frames.reset(); return; }
    if (!p.frames) p.frames = std::make_unique<clientSnapshot_t[]>(khz::SnapshotHistory);
    else std::fill_n(p.frames.get(), khz::SnapshotHistory, clientSnapshot_t{});
}
void KHZ_CommandIndex(client_t* client, unsigned index, unsigned count) {
    auto& p = peer(client);
    p.commandInput = p.packetInput >= count ? p.packetInput - count + index + 1 : 0;
}
void KHZ_QueueCommand(client_t* client, const usercmd_s* command) {
    if (KHZ_IsNativeBot(client)) return; // Native controls have no transport history.
    auto& p = peer(client);
    const auto sequence = client->header.netchan.remoteAddress.type == NA_BOT ? p.received + 1 : p.commandInput;
    if (command->serverTime > svs.time + 200 || command->serverTime < svs.time - 2000) { ++rejectedTime; return; }
    if (p.write - p.read == khz::InputHistory) { ++queueDrops; return; }
    if (!p.sequences.accept(sequence)) { ++duplicateCommands; return; }
    p.queue[p.write++ & (khz::InputHistory - 1)] = {*command, sequence, p.packetViewTick};
    p.received = p.sequences.highest(); ++receivedCommands; ++p.uniqueCommands;
    KHZ_AdminRecordCommand(client, sequence);
}
void KHZ_ApplyCommands() {
    KHZ_ServerCaptureBeginTick(session, serverTick, svs.time);
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        auto* client = &svs.clients[i]; if (client->header.state != CS_ACTIVE) continue;
        auto& p = peers[i];
        if (level.clients[i].sess.sessionState == SESS_STATE_PLAYING && g_entities[i].health > 0) ++p.playingTicks;
        usercmd_s selected{};
        const auto inputBefore = p.input.sequence(); unsigned consumedCommands = 0;
        if (KHZ_IsNativeBot(client)) {
            KHZ_BotBuildCommand(client, &selected);
            p.viewTick = serverTick;
            KHZ_AdminRecordCommand(client, serverTick);
        } else {
            if (!p.input.sequence()) p.input.reset(client->lastUsercmd);
            while (p.read != p.write) {
                const auto& item = p.queue[p.read++ & (khz::InputHistory - 1)];
                ++consumedCommands;
                if (item.sequence > p.input.sequence()) p.viewTick = item.viewTick;
                p.input.receive(item.command, item.sequence);
                p.applied = (std::max)(p.applied, item.sequence);
            }
            selected = p.input.consume();
        }
        selected.serverTime = svs.time;
        KHZ_PluginEvent(KHZPluginEvent::ClientMoveCommand, reinterpret_cast<std::uintptr_t>(client), reinterpret_cast<std::uintptr_t>(&selected));
        selected.serverTime = svs.time;
        KHZ_ServerCaptureRecord(client, &selected, inputBefore, p.input.sequence(), p.applied, p.received, p.viewTick, consumedCommands);
        client->lastUsercmd = selected;
        const auto viewAge = serverTick >= p.viewTick ? serverTick - p.viewTick : 0;
        G_SetLastServerTime(i, selected.serverTime - static_cast<int>(viewAge <= 250 ? viewAge : 0));
        ClientThink(i); // synchronous mode stores the command; G_RunClient advances it once
    }
}
void KHZ_BeginServerTick() {
    tickStart = khz::counter(); ++serverTick;
    KHZ_ServerMetricsBeginTick(serverTick);
    KHZ_ServerPhaseScope preparation(KHZ_ServerPhase::Preparation);
    KHZ_PluginBeginTick(serverTick);
    KHZ_HostServerTick();
    KHZ_BotTick(serverTick);
}
std::uint64_t KHZ_ServerTickNumber() { return serverTick; }
std::uint64_t KHZ_CurrentServerSession() { return session; }
void KHZ_EndServerTick() {
    KHZ_ServerCaptureEndTick(session, serverTick, svs.time);
    KHZ_ServerMetricsEndTick();
    work.add(usec(khz::counter() - tickStart));
}
void KHZ_WriteClientHeader(msg_t* message) {
    ++commandsSent;
    const auto number = CL_GetLocalClientGlobals(0)->cmdNumber;
    writeHeader(message, {serverHeader.session, KHZ_IsClientSimulationThread() ? KHZ_ClientOwnerTickNumber() : frameClock.tick(), static_cast<std::uint64_t>(number < 0 ? 0 : number), serverHeader.tick});
}
khz::HeaderDisposition KHZ_ReadClientHeader(client_t* client, msg_t* message) {
    khz::WireHeader header;
    if (!readHeader(message, header)) return khz::HeaderDisposition::Invalid;
    const auto disposition = khz::checkClientHeader(header, session, serverTick);
    if (disposition == khz::HeaderDisposition::Valid) {
        auto& p = peer(client); p.packetInput = header.input; p.packetViewTick = header.ack;
    }
    return disposition;
}
void KHZ_WriteServerHeader(client_t* client, msg_t* message) {
    if (!session) KHZ_ResetServer();
    writeHeader(message, {session, serverTick, peer(client).applied, peer(client).received});
}
void KHZ_RecordSnapshotSend(client_t* client, unsigned bytes) {
    snapshotBytes += bytes;
    auto& p = peer(client); p.bytesSent += bytes;
    if (client->header.state == CS_ACTIVE && p.snapshotTick != serverTick) {
        p.snapshotTick = serverTick; ++p.uniqueSnapshots; ++sentSnapshots;
    }
}
bool KHZ_ReadServerHeader(msg_t* message) {
    khz::WireHeader header;
    const int headerStart = message->readcount, headerBit = message->bit;
    const auto lastInput = (std::max)(0, CL_GetLocalClientGlobals(0)->cmdNumber);
    if (!readHeader(message, header) || !khz::validServerHeader(header, static_cast<std::uint64_t>(lastInput))) return false;
    if (header.session != serverHeader.session) {
        if (KHZ_IsClientSimulationThread()) {
            message->readcount = headerStart; message->bit = headerBit;
            KHZ_ClientTransportDeferPayload(message, true);
            return false;
        }
        inputTime = 0; clientActive = false; predictionHeader = {};
        KHZ_ResetCommittedSnapshotHeaders(header.session);
        std::fill(commands.begin(), commands.end(), usercmd_s{});
        std::fill(packets.begin(), packets.end(), outPacket_t{});
        std::fill(snapshots.begin(), snapshots.end(), clSnapshot_t{});
        std::fill(archive.begin(), archive.end(), ClientArchiveData{});
        idlePhases.fill(AccumulatedPhase{});
    }
    serverHeader = header; return true;
}
clientSnapshot_t* KHZ_ServerFrames(client_t* client) { auto& p = peer(client); if (!p.frames) p.frames = std::make_unique<clientSnapshot_t[]>(khz::SnapshotHistory); return p.frames.get(); }
clSnapshot_t* KHZ_ClientSnapshots() { return snapshots.data(); }
outPacket_t* KHZ_ClientPackets() { return packets.data(); }
usercmd_s* KHZ_ClientCommands() { return commands.data(); }
entityState_s* KHZ_ClientEntities() { return entities.data(); }
clientState_s* KHZ_ClientStates() { return states.data(); }
ClientArchiveData* KHZ_ClientArchive() { return archive.data(); }
std::uint64_t KHZ_ScriptNow() { return scriptNow; }
unsigned KHZ_ScriptKey(std::uint64_t deadline) {
    const auto key = scriptCalendar.keyFor(deadline * 2);
    if (!key) Com_Error(ERR_DROP, "cod4x1khz: script deadline capacity exhausted");
    return key;
}
unsigned KHZ_ScriptCurrentKey() {
    // Notifications join the currently executing phase's bucket. During
    // frame-end, listeners must resume/rearm between separate notifications
    // in that same phase, rather than waiting for the whole bucket to finish.
    const auto key = scriptCalendar.keyFor(scriptNow * 2 + (scriptFrameEnd ? 1 : 0));
    if (!key) Com_Error(ERR_DROP, "cod4x1khz: script deadline capacity exhausted");
    return key;
}
unsigned KHZ_ScriptDelayKey(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1e12) { Scr_Error("invalid or excessive wait duration"); return 0; }
    return KHZ_ScriptKey(scriptNow + khz::waitTicks(seconds));
}
unsigned KHZ_ScriptDelayKey(float seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1e12f) { Scr_Error("invalid or excessive wait duration"); return 0; }
    return KHZ_ScriptKey(scriptNow + khz::waitTicks(seconds));
}
unsigned KHZ_ScriptFrameEndKey() {
    const auto key = scriptCalendar.keyFor((scriptNow + (scriptFrameEnd ? 1 : 0)) * 2 + 1);
    if (!key) Com_Error(ERR_DROP, "cod4x1khz: script deadline capacity exhausted");
    return key;
}
void KHZ_ScriptBeginFrameEnd() { scriptFrameEnd = true; }
void KHZ_ScriptEndFrameEnd() { scriptFrameEnd = false; }
bool KHZ_ScriptIsCurrentKey(unsigned key) { return scriptCalendar.deadlineFor(key) / 2 == scriptNow; }
unsigned KHZ_ScriptTakeDue() { return scriptCalendar.takeDue(scriptNow * 2 + (scriptFrameEnd ? 1 : 0)); }
void KHZ_ScriptRelease(unsigned key) { scriptCalendar.release(key); }
void KHZ_ScriptReset() {
    scriptNow = 0; scriptFrameEnd = false; scriptCalendar.reset();
    if (KHZ_ScriptTestsEnabled()) scriptNow = 0xFFFFF0ULL;
}
bool KHZ_ScriptTestsEnabled() { return Dvar_GetBool("khz_script_tests"); }
const char* KHZ_ScriptProbeSource() {
    return R"gsc(main()
{
    setDvar("khz_script_probe", "running");
    wait 0.01;
    durations = [];
    durations[0] = 0; durations[1] = 0.001; durations[2] = 0.05;
    durations[3] = 0.1; durations[4] = 0.0011;
    expected = []; expected[0] = 1; expected[1] = 1; expected[2] = 50;
    expected[3] = 100; expected[4] = 2;
    for (i = 0; i < durations.size; i++)
    {
        start = getTime();
        wait durations[i];
        if (getTime() - start != expected[i])
        {
            setDvar("khz_script_probe", "failed wait " + i + " elapsed " + (getTime() - start));
            return;
        }
    }
    start = getTime();
    waittillframeend;
    if (getTime() != start) { setDvar("khz_script_probe", "failed frame end"); return; }
    waittillframeend;
    if (getTime() - start != 1) { setDvar("khz_script_probe", "failed repeated frame end"); return; }
    level.khz_notify_count = 0;
    level thread notify_listener();
    for (i = 0; i < 4; i++) level thread notify_once(i);
    wait 0.01;
    if (level.khz_notify_count != 4) {
        setDvar("khz_script_probe", "failed repeated frame-end notify count " + level.khz_notify_count); return;
    }
    setDvar("khz_script_probe", "passed");
}
notify_listener()
{
    while (level.khz_notify_count < 4) {
        level waittill("khz_probe_notify", value);
        level.khz_notify_count++;
    }
}
notify_once(value)
{
    waittillframeend;
    level notify("khz_probe_notify", value);
})gsc";
}
void KHZ_ScriptAdvance() { ++scriptNow; }
void KHZ_CaptureRewind() {
    auto& frame = rewindPoses.begin(serverTick);
    for (int i = 0; i < level.maxclients; ++i) {
        const auto& entity = g_entities[i]; const auto& client = level.clients[i];
        auto& pose = frame[i];
        pose.playing = entity.r.inuse && entity.health > 0 && client.sess.connected == CON_CONNECTED && client.sess.sessionState == SESS_STATE_PLAYING;
        if (!pose.playing) continue;
        pose.generation = entity.useCount; pose.spawnTime = client.lastSpawnTime;
        std::memcpy(pose.origin, entity.r.currentOrigin, sizeof(pose.origin));
        std::memcpy(pose.mins, entity.r.mins, sizeof(pose.mins));
        std::memcpy(pose.maxs, entity.r.maxs, sizeof(pose.maxs));
    }
}
bool KHZ_RewindClient(int client, int gameTime, float* origin, float* mins, float* maxs) {
    ++rewindQueries;
    const auto age = std::int64_t(level.time) - gameTime;
    if (client < 0 || client >= level.maxclients || age < 1 || age > 250 || std::uint64_t(age) > serverTick) return false;
    const auto* frame = rewindPoses.find(serverTick - std::uint64_t(age));
    if (!frame) return false;
    const auto& pose = (*frame)[client];
    if (!pose.playing || pose.generation != g_entities[client].useCount || pose.spawnTime != level.clients[client].lastSpawnTime) return false;
    std::memcpy(origin, pose.origin, sizeof(pose.origin));
    std::memcpy(mins, pose.mins, sizeof(pose.mins)); std::memcpy(maxs, pose.maxs, sizeof(pose.maxs)); ++rewindMatches; return true;
}
void KHZ_ScriptDecTime() { if (scriptNow) --scriptNow; }
unsigned KHZ_BobCycle(playerState_s* ps, double increment) {
    const auto advance = increment + ps->khzBobRemainder;
    const auto whole = std::floor(advance);
    ps->khzBobRemainder = static_cast<float>(advance - whole);
    return (ps->bobCycle + static_cast<int>(whole)) & 255;
}
void KHZ_AdvancePhase(int* phase, double increment) {
    const auto first = (reinterpret_cast<std::uintptr_t>(phase) >> 2) & (idlePhases.size() - 1);
    for (unsigned offset = 0; offset < idlePhases.size(); ++offset) {
        auto& entry = idlePhases[(first + offset) & (idlePhases.size() - 1)];
        if (!entry.owner) entry.owner = phase;
        if (entry.owner == phase) { entry.phase.advance(*phase, increment); return; }
    }
    Com_Error(ERR_DROP, "cod4x1khz: fractional phase capacity exhausted");
}
void KHZ_RawInput(std::uintptr_t handle) {
    RAWINPUT input{}; UINT size = sizeof(input);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(handle), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (input.header.dwType != RIM_TYPEMOUSE || (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) || !g_wv.activeApp) return;
    if (!mouseQueue.push({input.data.mouse.lLastX, input.data.mouse.lLastY, khz::counter()})) ++mouseDrops;
}
bool KHZ_RawMouse(int* dx, int* dy) {
    if (KHZ_InputOwnerOwnsRawDevices()) { *dx = *dy = 0; return true; }
    if (!rawRegistered) {
        RAWINPUTDEVICE device{1, 2, 0, g_wv.hWnd};
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) return false;
        rawRegistered = true;
    }
    *dx = *dy = 0; MouseSample sample;
    const auto cutoff = clientActive ? frameStart : khz::counter();
    while (mouseQueue.popDue(sample, cutoff)) { *dx += sample.dx; *dy += sample.dy; }
    return true;
}
void KHZ_DiscardRawMouse() { MouseSample sample; while (mouseQueue.pop(sample)) {} }
bool KHZ_QueueKey(unsigned time, int key, int down) {
#ifndef KISAK_DEDICATED
    if (!clientActive) return false;
    if (!keyQueue.push({time ? time : unsigned(Sys_Milliseconds()), key, down, khz::counter()})) {
        ++keyDrops; keyOverflow.store(true, std::memory_order_release);
    }
    return true;
#else
    return false;
#endif
}
void KHZ_PumpKeys() {
#ifndef KISAK_DEDICATED
    KeySample sample;
    if (keyOverflow.exchange(false, std::memory_order_acq_rel)) {
        while (keyQueue.pop(sample)) {}
        // Overflow cannot leave a lost key-up holding an action indefinitely.
        Key_ClearStates(0); return;
    }
    const auto cutoff = clientActive ? frameStart : khz::counter();
    while (keyQueue.popDue(sample, cutoff)) CL_KeyEvent(0, sample.key, sample.down, sample.time);
#endif
}
