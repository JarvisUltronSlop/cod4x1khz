// STAGED ONLY: END khz_screenshot_server.cpp. Its normal idle worker exists from
// initialization; only actual pending/task phases are excluded. Free slots have
// no independent writer, and the sole main owner cannot enqueue during probe.
#include "khz_archive_probe_boundary.h"
bool KHZ_ArchiveProbeScreenshotEligible(char* reason, unsigned capacity) {
    if (!Sys_IsMainThread()) return KHZ_ArchiveProbeRefuse(reason, capacity, "screenshot_owner_thread_required");
    if (!state) return true;
    if (state->stopping.load(std::memory_order_acquire) || state->read != state->write)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "screenshot_queue_or_lifecycle_pending");
    for (const auto& slot : state->slots) if (slot.phase.load(std::memory_order_acquire) != Phase::Free)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "screenshot_transfer_or_worker_task");
    return true;
}
