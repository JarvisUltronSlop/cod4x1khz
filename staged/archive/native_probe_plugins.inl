// STAGED ONLY: END khz_plugins.cpp. Requires Registry::LoadGeneration() accessor
// over actual persistent generation_; FinishUnload clears per-slot generation.
#include "native_probe_boundary.h"
bool KHZ_ArchiveProbePluginsEligible(char* reason, unsigned capacity) {
    if (!Sys_IsMainThread()) return KHZ_ArchiveProbeRefuse(reason, capacity, "plugin_owner_thread_required");
    if (registry.LoadGeneration() || registry.Loaded() || activePlugin != -1 ||
        authoritativeCallback || networkCallback || shuttingDown.load(std::memory_order_acquire))
        return KHZ_ArchiveProbeRefuse(reason, capacity, "plugin_load_history_or_callback");
    for (const auto& module : modules) if (module.handle || module.importCount)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "plugin_module_or_import_pin");
    for (const auto& job : jobs) if (job.state.load(std::memory_order_acquire) != Free)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "plugin_job_pending");
    if (commandCount || logCount || workerLogCount)
        return KHZ_ArchiveProbeRefuse(reason, capacity, "plugin_service_pending");
    return true;
}
