// STAGED ONLY: END khz_host.cpp, dedicated custom target. The preparation
// proposal inserts private prototypes before handleServer and explicit begin /
// end around the validated HostMessage::Command dispatch. No public setter.
#include "native_probe_boundary.h"
#include <qcommon/cmd.h>
#if defined(COD4X1KHZ_SERVER_CHECKPOINT)
#include "khz_server_checkpoint.h"
#endif

namespace {
KHZ_ArchiveProbeOwnedCommand archiveOwnedCommand;
bool archiveOwnedCommandActive = false;
bool archiveOwnedCommandIsDirectProbe = false;
cmd_function_s archiveCodecProbeCommand;

bool beginArchiveProbeOwnedCommand(const Packet& value) {
    // Ordinary validated host administration retains its original behavior.
    // No scope is established for exec/vstr/map/status or their nested calls.
    if (std::strcmp(value.command, "khz_archive_probe")) return true;
    if (!Sys_IsMainThread() || archiveOwnedCommandActive ||
        value.message != HostMessage::Command || value.sequence != host.receivedSequence ||
        value.pid != host.parentPid || !sameToken(value.token) ||
        !host.initialized || !host.managedServer || !host.authenticated || !host.configured ||
        host.phase != Phase::Ready || host.pendingReady || host.quitQueued ||
        host.checkpointPaused || host.checkpointRequest ||
        !host.channel.connected || !host.channel.peerVerified || host.channel.failed ||
        com_inServerFrame || !KHZ_CurrentServerSession()) return false;
    archiveOwnedCommand.session = KHZ_CurrentServerSession();
    archiveOwnedCommand.tick = KHZ_ServerTickNumber();
    archiveOwnedCommand.parentPid = host.parentPid;
    archiveOwnedCommand.sequence = value.sequence;
    archiveOwnedCommand.serverId = sv_serverId_value;
    // A nested exec/vstr/admin command does not inherit codec permission.
    archiveOwnedCommandIsDirectProbe = !std::strcmp(value.command, "khz_archive_probe");
    archiveOwnedCommandActive = true;
    return true;
}
void endArchiveProbeOwnedCommand() {
    archiveOwnedCommandActive = archiveOwnedCommandIsDirectProbe = false;
    archiveOwnedCommand = {};
}
void archiveCodecProbe_f() {
    KHZ_ArchiveProbeOwnedCommand origin;
    // Do not run the native codec on public RCON, a script, or a nested command.
    // Passive failure text does not expose a saved result as a new execution.
    if (Cmd_Argc() != 1 || !KHZ_ArchiveProbeOwnedCommandIdentity(&origin)) {
        Com_Printf(CON_CHANNEL_DONT_FILTER,
            "KHZ_ARCHIVE_PROBE_REFUSED authenticated_direct_owned_command_required\n");
        return;
    }
    const ULONGLONG started = GetTickCount64();
    KHZ_ArchiveRunCodecProbe();
    const ULONGLONG elapsed = GetTickCount64() - started;
    const char* report = KHZ_ArchiveCodecProbeJSON();
    // HostMessage output is 2048 bytes. Fail rather than publish partial JSON.
    if (!report || std::strlen(report) > 1550) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_ARCHIVE_PROBE_REFUSED report_capacity\n");
        return;
    }
    Com_Printf(CON_CHANNEL_DONT_FILTER,
        "KHZ_ARCHIVE_PROBE {\"origin\":{\"direct_owned_command\":true,\"server_pid\":%lu,"
        "\"parent_pid\":%u,\"request_sequence\":%u,\"session\":%llu,\"tick\":%llu,\"server_id\":%d},"
        "\"native_owner_wall_ms\":%llu,\"codec\":%s}\n",
        GetCurrentProcessId(), origin.parentPid, origin.sequence, origin.session,
        origin.tick, origin.serverId, elapsed, report);
}
}

bool KHZ_ArchiveProbeOwnedCommandIdentity(KHZ_ArchiveProbeOwnedCommand* output) {
    if (!Sys_IsMainThread() || !output || !archiveOwnedCommandActive ||
        !archiveOwnedCommandIsDirectProbe || !host.initialized || !host.managedServer ||
        !host.authenticated || !host.configured || host.phase != Phase::Ready ||
        host.pendingReady || host.quitQueued || host.checkpointPaused || host.checkpointRequest ||
        !host.channel.connected || !host.channel.peerVerified || host.channel.failed ||
        archiveOwnedCommand.parentPid != host.parentPid ||
        archiveOwnedCommand.sequence != host.receivedSequence ||
        archiveOwnedCommand.session != KHZ_CurrentServerSession() ||
        archiveOwnedCommand.tick != KHZ_ServerTickNumber() ||
        archiveOwnedCommand.serverId != sv_serverId_value || com_inServerFrame) return false;
    *output = archiveOwnedCommand;
    return true;
}
void KHZ_ArchiveProbeOwnedCommandUnwind() {
    // Actual main error cleanup calls this before abandoning a redirect scope;
    // SV_Shutdown repeats it after the codec abort and before final messages.
    if (Sys_IsMainThread()) endArchiveProbeOwnedCommand();
}
bool KHZ_ArchiveProbePrivateBoundary(char* reason, unsigned capacity) {
    if (reason && capacity) reason[0] = 0;
    KHZ_ArchiveProbeOwnedCommand origin;
    if (!KHZ_ArchiveProbeOwnedCommandIdentity(&origin))
        return KHZ_ArchiveProbeRefuse(reason, capacity, "authenticated_direct_owned_command_required");
    if (!Dvar_GetBool("khz_benchmark") || !Dvar_GetBool("khz_test_controls"))
        return KHZ_ArchiveProbeRefuse(reason, capacity, "benchmark_and_test_controls_required");
    if (host.pendingRequest || host.checkpointPaused ||
        host.checkpointRequest || host.pendingReady || host.quitQueued || host.phase != Phase::Ready)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "owned_host_lifecycle_or_checkpoint_pending");
#if defined(COD4X1KHZ_SERVER_CHECKPOINT)
    if (KHZ_ServerCheckpointPinned())
        return KHZ_ArchiveProbeRefuse(reason, capacity, "persistent_world_checkpoint_pinned");
#endif
    return KHZ_ArchiveProbePluginsEligible(reason, capacity) &&
        KHZ_ArchiveProbeContentEligible(reason, capacity) &&
        KHZ_ArchiveProbeContentServerEligible(reason, capacity) &&
        KHZ_ArchiveProbeWebEligible(reason, capacity) &&
        KHZ_ArchiveProbeScreenshotEligible(reason, capacity) &&
        KHZ_ArchiveProbeAdminEligible(reason, capacity) &&
        KHZ_ArchiveProbeCommandQueuesEligible(reason, capacity);
}
bool KHZ_InitArchiveCodecProbeCommand() {
    if (!Sys_IsMainThread()) return false;
    // Install the real callback before exposing a command. Registration itself
    // permits no codec access; every invocation verifies flags and origin.
    if (!KHZ_ArchiveProbeInstallBoundary(KHZ_ArchiveProbePrivateBoundary)) return false;
    Cmd_AddCommandInternal("khz_archive_probe", archiveCodecProbe_f, &archiveCodecProbeCommand);
    return true;
}
