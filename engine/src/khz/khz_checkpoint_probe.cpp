#include <universal/q_shared.h>
#include "khz_checkpoint_probe.h"
#include "khz_client_checkpoint.h"
#include "khz_client_transport.h"
#include "khz_client_owner.h"
#include "khz_client_input.h"
#include "khz_input_owner.h"
#include "khz_host.h"
#include "khz_runtime.h"
#include <qcommon/threads.h>
#include <qcommon/qcommon.h>
#include <cstdio>
#include <cstring>

const char* KHZ_ClientCheckpointProbe(const char* action) {
    iassert(Sys_IsMainThread());
    static char result[4096], observed[2048]; static std::uint64_t fixture = 0;
    if (!action || std::strcmp(action, "roundtrip")) {
        std::snprintf(result, sizeof(result), "{\"result\":\"%s\",\"checkpoint\":%s}",
            action && !std::strcmp(action, "status") ? "queried" : "unknown action", KHZ_ClientCheckpointStatusJSON());
        return result;
    }
    if (!Dvar_GetBool("khz_benchmark") || !Dvar_GetBool("khz_test_controls") || !KHZ_HasOwnedHost())
        return "{\"result\":\"rejected\",\"reason\":\"requires an isolated benchmark and its owned server\"}";
    if (!++fixture) ++fixture;
    const auto session = KHZ_AcceptedClientSession();
    const char* stage = "transport fence";
    bool okay = KHZ_ClientTransportBeginCheckpointFence(fixture);
    // Preserve the rejection before cleanup can issue a resume request.
    char fenceFailure[320]{};
    if (!okay) {
        const char* diagnostic = KHZ_HostCheckpointFailure(fixture);
        if (!*diagnostic) diagnostic = "transport_rejected_before_host_reply_or_drain";
        unsigned length = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(diagnostic); *p && length + 2 < sizeof(fenceFailure); ++p) {
            if (*p == '"' || *p == '\\') fenceFailure[length++] = '\\';
            fenceFailure[length++] = *p < 32 ? ' ' : char(*p);
        }
        fenceFailure[length] = 0;
    }
    if (okay) { stage = "capture"; okay = KHZ_ClientCheckpointCapture(fixture); }
    if (okay) { stage = "validation"; okay = KHZ_ClientCheckpointValidateRestore(); }
    if (okay) { stage = "restore"; okay = KHZ_ClientCheckpointRestore(); }
    std::snprintf(observed, sizeof(observed), "%s", KHZ_ClientCheckpointStatusJSON());
    if (okay) { stage = "presentation rewind"; okay = KHZ_ClientOwnerCheckpointResetPresentation(); }
    if (okay) { stage = "discard"; okay = KHZ_ClientCheckpointDiscard(); }
    if (okay) { stage = "server resume"; okay = KHZ_ClientTransportEndCheckpointFence(fixture); }
    if (okay) { stage = "client resume"; okay = KHZ_ClientOwnerResumeCheckpoint(); }
    bool cleanup = true;
    if (!okay) {
        // A rejected/partial restore cannot resume a private image. Join the
        // real owner, which releases its pinned native references, then ask
        // the authenticated owned server to leave only this fixture's fence.
        KHZ_ClientSimulationShutdown();
        cleanup = !KHZ_ClientOwnerRunning() && KHZ_ClientCheckpointCanReleaseAssets();
        if (KHZ_HostCheckpointFenced(fixture, session)) cleanup = KHZ_HostCheckpointResume(fixture) && cleanup;
    } else {
        // No receipt tape was started: return control to the actual hardware
        // producer only after the restored consumer acknowledged its resume.
        KHZ_InputOwnerSetGameplay(KHZ_ClientGameplayInputAllowed(), KHZ_ClientOwnerGeneration());
        KHZ_InputOwnerResume();
    }
    std::snprintf(result, sizeof(result),
        "{\"result\":\"%s\",\"stage\":\"%s\",\"fixture\":%llu,\"cleanup\":%s,\"full_world_replay\":false,\"reason\":\"%s\",\"checkpoint\":%s}",
        okay ? "restored" : "rejected", okay ? "complete" : stage, fixture, cleanup ? "true" : "false", fenceFailure, observed);
    return result;
}
