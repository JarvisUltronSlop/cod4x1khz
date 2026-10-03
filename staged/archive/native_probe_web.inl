// STAGED ONLY: END khz_webadmin.cpp. Reject a constructed thread even before its
// running publication, and all retained owner/worker request phases.
#include "native_probe_boundary.h"
#include <qcommon/threads.h>
bool KHZ_ArchiveProbeWebEligible(char* reason, unsigned capacity) {
    if (!Sys_IsMainThread()) return KHZ_ArchiveProbeRefuse(reason, capacity, "web_owner_thread_required");
    if (worker.joinable() || running.load(std::memory_order_acquire))
        return KHZ_ArchiveProbeRefuse(reason, capacity, "web_worker_running_or_starting");
    for (const auto& slot : slots) if (slot.phase.load(std::memory_order_acquire) != Free)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "web_request_pending");
    return true;
}
