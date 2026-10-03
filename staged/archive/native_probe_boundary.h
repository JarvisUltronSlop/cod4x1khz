#pragma once
#include "native_probe.h"
#include <cstdint>
#include <cstdio>

// STAGED ONLY. Each helper is defined at the END of its actual owner TU.
// No diagnostic JSON, foreign-thread access to owner fields, or queue pumping.
struct KHZ_ArchiveProbeOwnedCommand {
    std::uint64_t session = 0, tick = 0;
    unsigned parentPid = 0, sequence = 0;
    int serverId = 0;
};
bool KHZ_ArchiveProbeOwnedCommandIdentity(KHZ_ArchiveProbeOwnedCommand*);
void KHZ_ArchiveProbeOwnedCommandUnwind();
bool KHZ_ArchiveProbePluginsEligible(char*, unsigned);
bool KHZ_ArchiveProbeContentServerEligible(char*, unsigned);
bool KHZ_ArchiveProbeContentEligible(char*, unsigned);
bool KHZ_ArchiveProbeScreenshotEligible(char*, unsigned);
bool KHZ_ArchiveProbeWebEligible(char*, unsigned);
bool KHZ_ArchiveProbeAdminEligible(char*, unsigned);
bool KHZ_ArchiveProbeCommandQueuesEligible(char*, unsigned);
bool KHZ_ArchiveProbePrivateBoundary(char*, unsigned);
bool KHZ_InitArchiveCodecProbeCommand();

inline bool KHZ_ArchiveProbeRefuse(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text);
    return false;
}
