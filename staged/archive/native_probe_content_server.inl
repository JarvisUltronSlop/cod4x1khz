// STAGED ONLY: END khz_content_server.cpp. start marks worker-ever-started BEFORE
// its actual std::thread construction; lifecycle cleanup never clears history.
#include "native_probe_boundary.h"
bool KHZ_ArchiveProbeContentServerEligible(char* reason, unsigned capacity) {
    if (!Sys_IsMainThread()) return KHZ_ArchiveProbeRefuse(reason, capacity, "content_server_owner_thread_required");
    if (archiveProbeWorkerEverStarted || worker.joinable())
        return KHZ_ArchiveProbeRefuse(reason, capacity, "content_server_worker_history");
    // This deliberately excludes even accepted-but-not-yet-dequeued requests,
    // including a request whose resolution failed before a worker was started.
    if (counters.accepted || jobRead != jobWrite || resultRead != resultWrite)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "content_server_request_history");
    for (const auto& rejection : errors) if (rejection.waiting)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "content_server_error_pending");
    return true;
}
