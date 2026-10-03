// STAGED ONLY: END qcommon/cmd.cpp. Do not scan command strings or infer whether
// pending text might mutate a map: exclude actual nonempty native owner queues.
#include "native_probe_boundary.h"
bool KHZ_ArchiveProbeCommandQueuesEligible(char* reason, unsigned capacity) {
    if (!Sys_IsMainThread()) return KHZ_ArchiveProbeRefuse(reason, capacity, "command_owner_thread_required");
    if (sv_cmd_text.cmdsize || cmd_wait)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "server_command_or_wait_pending");
    for (unsigned i = 0; i < ARRAY_COUNT(cmd_textArray); ++i)
        if (cmd_textArray[i].cmdsize || cmd_insideCBufExecute[i])
            return KHZ_ArchiveProbeRefuse(reason, capacity, "native_command_queue_or_execution_pending");
    return true;
}
