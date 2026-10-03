// STAGED ONLY: END khz_admin_server.cpp AFTER khz_webadmin_adapter.inl, so the
// source-owned actual webRedirect/configuration and native invoker are visible.
#include "native_probe_boundary.h"
bool KHZ_ArchiveProbeAdminEligible(char* reason, unsigned capacity) {
    if (!Sys_IsMainThread()) return KHZ_ArchiveProbeRefuse(reason, capacity, "admin_owner_thread_required");
    if (requestCount || pluginBanCount || auditCount || webRedirect ||
        redirectSlot != -1 || remoteConsoleContext || !currentInvoker.console ||
        (webEnabled && webEnabled->current.enabled))
        return KHZ_ArchiveProbeRefuse(reason, capacity, "admin_command_service_or_web_transition_pending");
    return true;
}
