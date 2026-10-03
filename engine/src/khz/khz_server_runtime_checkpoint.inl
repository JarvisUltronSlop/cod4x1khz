// Authoritative custom queues, input latches, baselines, rewind and fractional
// phases. Native sv/svs and script calendar/RNG have independent mandatory parts.
#include "khz_server_runtime_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include <type_traits>

namespace {
using namespace khz::server_native_checkpoint;
struct ServerPeerImage {
    decltype(Peer::queue) queue;
    decltype(Peer::sequences) sequences;
    decltype(Peer::input) input;
    unsigned read, write;
    std::uint64_t received, applied, packetInput, commandInput, packetViewTick, viewTick;
    std::uint64_t snapshotTick, uniqueSnapshots, uniqueCommands, bytesSent, playingTicks;
    clientSnapshot_t* originalFrames;
    void* baselineImage;
};
struct ServerRuntimeImage {
    KHZ_ServerCheckpointFrame frame;
    ServerPeerImage peer[64];
    Bytes<sizeof(rewindPoses)> rewind;
    Bytes<sizeof(idlePhases)> phases;
    Bytes<sizeof(work)> work, lateness;
    khz::DeadlineClock clock, preparedClock;
    std::uint64_t statisticsElapsed, preparedNow;
    std::uint64_t receivedCommands, sentSnapshots, snapshotBytes, rewindQueries, rewindMatches;
    std::uint64_t backlogMax, rejectedTime, duplicateCommands, queueDrops;
    bool captured;
};
// Metadata only carries a small fixed BSS image; the substantial per-human
// baselines are allocated only by explicit paused fixture capture.
ServerRuntimeImage serverRuntimeImage;
constexpr std::size_t ServerBaselineBytes = sizeof(clientSnapshot_t) * khz::SnapshotHistory;
static_assert(std::is_trivially_copyable_v<decltype(rewindPoses)>);
static_assert(std::is_trivially_copyable_v<decltype(idlePhases)>);
static_assert(std::is_trivially_copyable_v<decltype(Peer::queue)>);
static_assert(std::is_trivially_copyable_v<decltype(Peer::sequences)>);
static_assert(std::is_trivially_copyable_v<decltype(Peer::input)>);

bool serverRuntimeBoundary(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity)) return false;
    if (session != f.session || serverTick != f.logicalTick || svs.time != f.nativeTime ||
        scriptFrameEnd || !frameClock.frequency() || frameClock.frequency() != khz::frequency())
        return reject(text, capacity, "server_runtime_not_at_completed_native_tick");
    for (unsigned i = 0; i != 64; ++i) {
        const auto& p = peers[i];
        if (p.write - p.read > khz::InputHistory || p.applied > p.received ||
            p.input.sequence() > p.applied || p.received != p.sequences.highest())
            return reject(text, capacity, "server_runtime_input_queue_or_acknowledgement_is_invalid");
        if (KHZ_IsNativeBot(&svs.clients[i]) && p.frames)
            return reject(text, capacity, "server_runtime_native_bot_has_transport_baselines");
        for (auto cursor = p.read; cursor != p.write; ++cursor) {
            const auto& command = p.queue[cursor & (khz::InputHistory - 1)];
            if (!command.sequence || command.sequence > p.received || command.viewTick > serverTick)
                return reject(text, capacity, "server_runtime_queued_input_identity_is_invalid");
        }
    }
    for (const auto& phase : idlePhases)
        if (phase.owner && !KHZ_ServerCheckpointRestoresAddress(phase.owner, sizeof(*phase.owner)))
            return reject(text, capacity, "server_runtime_fractional_phase_points_outside_restored_native_storage");
    return true;
}
void serverRuntimeDiscard() {
    serverRuntimeImage.captured = false;
    for (auto& p : serverRuntimeImage.peer) {
        if (p.baselineImage) VirtualFree(p.baselineImage, 0, MEM_RELEASE);
        p.baselineImage = nullptr; p.originalFrames = nullptr;
    }
}
bool serverRuntimeCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverRuntimeBoundary(f, text, capacity)) return false;
    auto& image = serverRuntimeImage;
    for (const auto& saved : image.peer)
        if (saved.baselineImage) return reject(text, capacity, "server_runtime_baseline_image_already_pinned");
    image.frame = f;
    for (unsigned i = 0; i != 64; ++i) {
        const auto& p = peers[i]; auto& saved = image.peer[i];
        saved.queue = p.queue; saved.sequences = p.sequences; saved.input = p.input;
        saved.read = p.read; saved.write = p.write;
        saved.received = p.received; saved.applied = p.applied;
        saved.packetInput = p.packetInput; saved.commandInput = p.commandInput;
        saved.packetViewTick = p.packetViewTick; saved.viewTick = p.viewTick;
        saved.snapshotTick = p.snapshotTick; saved.uniqueSnapshots = p.uniqueSnapshots;
        saved.uniqueCommands = p.uniqueCommands; saved.bytesSent = p.bytesSent; saved.playingTicks = p.playingTicks;
        saved.originalFrames = p.frames.get();
        if (saved.originalFrames) {
            saved.baselineImage = VirtualAlloc(nullptr, ServerBaselineBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!saved.baselineImage) return reject(text, capacity, "server_runtime_baseline_image_allocation_failed");
            std::memcpy(saved.baselineImage, saved.originalFrames, ServerBaselineBytes);
        }
    }
    image.rewind.capture(&rewindPoses); image.phases.capture(&idlePhases);
    image.work.capture(&work); image.lateness.capture(&lateness); image.clock = frameClock;
    const auto now = khz::counter();
    if (statsStart > now) return reject(text, capacity, "server_runtime_statistics_epoch_is_invalid");
    image.statisticsElapsed = now - statsStart;
    image.receivedCommands = receivedCommands; image.sentSnapshots = sentSnapshots; image.snapshotBytes = snapshotBytes;
    image.rewindQueries = rewindQueries; image.rewindMatches = rewindMatches;
    image.backlogMax = backlogMax; image.rejectedTime = rejectedTime;
    image.duplicateCommands = duplicateCommands; image.queueDrops = queueDrops;
    image.captured = true; return true;
}
bool serverRuntimeValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverRuntimeBoundary(f, text, capacity)) return false;
    auto& image = serverRuntimeImage;
    if (!image.captured || !khz::server_checkpoint::sameLifetime(f, image.frame))
        return reject(text, capacity, "server_runtime_original_map_owner_or_fence_changed");
    for (unsigned i = 0; i != 64; ++i)
        if (peers[i].frames.get() != image.peer[i].originalFrames ||
            bool(image.peer[i].originalFrames) != bool(image.peer[i].baselineImage))
            return reject(text, capacity, "server_runtime_baseline_allocation_lifetime_changed");
    image.preparedNow = khz::counter();
    if (image.preparedNow < image.statisticsElapsed ||
        !image.preparedClock.rebase(image.preparedNow, image.clock.frequency(), image.clock.tick()))
        return reject(text, capacity, "server_runtime_wall_clock_rebase_would_overflow");
    return true;
}
void serverRuntimeRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext() && serverRuntimeImage.captured);
    const auto& image = serverRuntimeImage;
    for (unsigned i = 0; i != 64; ++i) {
        auto& p = peers[i]; const auto& saved = image.peer[i];
        // unique_ptr ownership never gets copied, reset or reconstructed.
        p.queue = saved.queue; p.sequences = saved.sequences; p.input = saved.input;
        p.read = saved.read; p.write = saved.write;
        p.received = saved.received; p.applied = saved.applied;
        p.packetInput = saved.packetInput; p.commandInput = saved.commandInput;
        p.packetViewTick = saved.packetViewTick; p.viewTick = saved.viewTick;
        p.snapshotTick = saved.snapshotTick; p.uniqueSnapshots = saved.uniqueSnapshots;
        p.uniqueCommands = saved.uniqueCommands; p.bytesSent = saved.bytesSent; p.playingTicks = saved.playingTicks;
        if (saved.originalFrames) std::memcpy(saved.originalFrames, saved.baselineImage, ServerBaselineBytes);
    }
    image.rewind.restore(&rewindPoses); image.phases.restore(&idlePhases);
    image.work.restore(&work); image.lateness.restore(&lateness);
    serverTick = image.frame.logicalTick; session = image.frame.session;
    frameClock = image.preparedClock; frameStart = frameClock.deadline(); tickStart = image.preparedNow;
    statsStart = image.preparedNow - image.statisticsElapsed;
    receivedCommands = image.receivedCommands; sentSnapshots = image.sentSnapshots; snapshotBytes = image.snapshotBytes;
    rewindQueries = image.rewindQueries; rewindMatches = image.rewindMatches;
    backlogMax = image.backlogMax; rejectedTime = image.rejectedTime;
    duplicateCommands = image.duplicateCommands; queueDrops = image.queueDrops;
}
}
void KHZ_ServerRuntimeRegisterCheckpoint() {
    if (!KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Runtime, &rewindPoses, sizeof(rewindPoses)) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Runtime, &idlePhases, sizeof(idlePhases)))
        Com_Error(ERR_FATAL, "Native server runtime checkpoint ranges could not register");
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Runtime, 0,
        sizeof(serverRuntimeImage) + ServerBaselineBytes * 64,
        serverRuntimeCapture, serverRuntimeValidate, serverRuntimeRestore, serverRuntimeDiscard};
    if (!KHZ_ServerCheckpointRegister(p)) Com_Error(ERR_FATAL, "Duplicate native server runtime checkpoint provider");
}
bool KHZ_ServerRuntimeResumeCheckpointClock(char* text, unsigned capacity) {
    if (!Sys_IsMainThread() || !com_dedicated || !com_dedicated->current.integer ||
        !Dvar_GetBool("khz_benchmark") || !KHZ_ServerCheckpointPaused() || com_inServerFrame || scriptFrameEnd)
        return khz::server_native_checkpoint::reject(text, capacity, "server_checkpoint_resume_requires_actual_idle_authenticated_halt");
    const auto now = khz::counter();
    auto resumed = frameClock;
    if (!resumed.frequency() || resumed.frequency() != khz::frequency() ||
        !resumed.rebase(now, resumed.frequency(), resumed.tick()))
        return khz::server_native_checkpoint::reject(text, capacity, "server_checkpoint_resume_clock_would_overflow");
    // Runtime validation occurs before potentially large native archive copies.
    // It cannot choose the final resume deadline for the whole coordinator.
    frameClock = resumed; frameStart = resumed.deadline(); tickStart = now;
    return true;
}
