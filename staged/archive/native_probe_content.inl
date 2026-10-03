// STAGED ONLY: END khz_content.cpp. Start sets archiveProbeWorkerEverStarted
// atomically BEFORE constructing a thread; Shutdown never clears that history.
#include "native_probe_boundary.h"
#include <qcommon/threads.h>
namespace khz::content {
bool ArchiveProbeEligible(char* reason, unsigned capacity) {
    if (!Sys_IsMainThread()) return KHZ_ArchiveProbeRefuse(reason, capacity, "content_owner_thread_required");
    if (archiveProbeWorkerEverStarted.load(std::memory_order_acquire) ||
        running.load(std::memory_order_acquire) || worker.joinable())
        return KHZ_ArchiveProbeRefuse(reason, capacity, "content_worker_history");
    // No worker has ever existed, so these sole-owner rings cannot change
    // concurrently. GetProgress.running is also a real typed public observation.
    const auto progress = GetProgress();
    if (progress.running || progress.id || progress.queued ||
        requestRead != requestWrite || completionRead != completionWrite)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "content_service_pending");
    return true;
}
}
bool KHZ_ArchiveProbeContentEligible(char* reason, unsigned capacity) {
    return khz::content::ArchiveProbeEligible(reason, capacity);
}
