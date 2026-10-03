// STAGED ONLY: END server_mp/sv_snapshot_profile_mp.cpp, dedicated custom build.
// Actual native source symbols, no mirror counters and no World provider claim.
#include "khz_archive_probe.h"
#include <cstring>
#include <type_traits>
#include <qcommon/threads.h>

namespace khz_archive_probe_profile {
// The field macro also enumerates capture/restore/match, preventing a member
// from being added to the image but accidentally omitted from commit.
#define KHZ_ARCHIVE_PROBE_PROFILE_FIELDS(X) \
    X(g_bitsSent) X(s_totalPacketDataSizes) X(s_packetMetaDataSize) \
    X(s_packetModeStart) X(s_packetMode) X(g_currentSnapshotPerEntity) \
    X(g_currentSnapshotFieldsPerEntity) X(g_currentSnapshotPlayerStateFields) \
    X(newDataReady) X(bitsUsedPerEType) X(bitsUsedForPlayerstates) \
    X(playerStateFieldsChanged) X(s_packetDataEnabled) X(s_floatBitsCompressed) \
    X(s_originDeltaBits) X(s_originZDeltaBits) X(s_originZFullBits) \
    X(s_originFullBits) X(networkEntityFieldsChanged) \
    X(currentSnapshotNetworkEntityFieldsChanged) X(bitsUsedForServerCommands) \
    X(s_currentEntType) X(s_currentEntNum) X(originsSentDueToPredicitonError) \
    X(originsSentDueToServerTimeMismatch) X(s_stdSnapshotDeviation) \
    X(s_avgSnapshotSize) X(s_maxSnapshotSize) X(s_numSnapshotSamples) \
    X(s_numSnapshotsBuiltSinceLastPoll) X(s_uncompressedDataSinceLastPoll) \
    X(s_compressedDataSinceLastPoll) X(s_numSnapshotsSentSinceLastPoll)
struct Image {
#define KHZ_ARCHIVE_PROBE_PROFILE_MEMBER(name) decltype(::name) name;
    KHZ_ARCHIVE_PROBE_PROFILE_FIELDS(KHZ_ARCHIVE_PROBE_PROFILE_MEMBER)
#undef KHZ_ARCHIVE_PROBE_PROFILE_MEMBER
};
static_assert(std::is_trivially_copyable_v<Image>);
}
std::size_t KHZ_ArchiveProbeProfileBytes() {
    return sizeof(khz_archive_probe_profile::Image);
}
bool KHZ_ArchiveProbeProfileCapture(void* storage, std::size_t capacity) {
    if (!Sys_IsMainThread() || !storage || capacity != sizeof(khz_archive_probe_profile::Image)) return false;
    auto* image = static_cast<khz_archive_probe_profile::Image*>(storage);
    std::memset(image, 0, sizeof(*image)); // deterministic own padding, not native mutation
#define KHZ_ARCHIVE_PROBE_PROFILE_COPY(name) std::memcpy(&image->name, &::name, sizeof(::name));
    KHZ_ARCHIVE_PROBE_PROFILE_FIELDS(KHZ_ARCHIVE_PROBE_PROFILE_COPY)
#undef KHZ_ARCHIVE_PROBE_PROFILE_COPY
    return true;
}
void KHZ_ArchiveProbeProfileRestore(const void* storage, std::size_t capacity) {
    iassert(Sys_IsMainThread() && storage && capacity == sizeof(khz_archive_probe_profile::Image));
    const auto* image = static_cast<const khz_archive_probe_profile::Image*>(storage);
#define KHZ_ARCHIVE_PROBE_PROFILE_RESTORE(name) std::memcpy(&::name, &image->name, sizeof(::name));
    KHZ_ARCHIVE_PROBE_PROFILE_FIELDS(KHZ_ARCHIVE_PROBE_PROFILE_RESTORE)
#undef KHZ_ARCHIVE_PROBE_PROFILE_RESTORE
}
bool KHZ_ArchiveProbeProfileMatches(const void* storage, std::size_t capacity) {
    if (!Sys_IsMainThread() || !storage || capacity != sizeof(khz_archive_probe_profile::Image)) return false;
    const auto* image = static_cast<const khz_archive_probe_profile::Image*>(storage);
#define KHZ_ARCHIVE_PROBE_PROFILE_MATCH(name) if (std::memcmp(&::name, &image->name, sizeof(::name))) return false;
    KHZ_ARCHIVE_PROBE_PROFILE_FIELDS(KHZ_ARCHIVE_PROBE_PROFILE_MATCH)
#undef KHZ_ARCHIVE_PROBE_PROFILE_MATCH
    return true;
}
#undef KHZ_ARCHIVE_PROBE_PROFILE_FIELDS
