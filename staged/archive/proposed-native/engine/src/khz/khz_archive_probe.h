#pragma once
#include <cstddef>
#include <cstdint>

// Installed only by an actual source-owned dedicated/private control adapter.
// It must reject loaded plugins, active service workers/transfers, an asset
// mutation, or a pinned persistent world checkpoint. Missing callback rejects.
using KHZ_ArchiveProbeBoundary = bool (*)(char* reason, unsigned capacity);
bool KHZ_ArchiveProbeInstallBoundary(KHZ_ArchiveProbeBoundary boundary);
bool KHZ_ArchiveRunCodecProbe();
const char* KHZ_ArchiveCodecProbeJSON();
bool KHZ_ArchiveCodecProbeActive();
// Mandatory immediately BEFORE ordinary SV_Shutdown can release the map/svs.
// This is explicit native longjmp cleanup, not a C++ destructor or world restore.
void KHZ_ArchiveProbeAbortBeforeServerShutdown();

// Source-owned EOF sv_snapshot_profile_mp.cpp fragment; caller supplies exact
// storage allocated before changing the codec. No persistent global image.
std::size_t KHZ_ArchiveProbeProfileBytes();
bool KHZ_ArchiveProbeProfileCapture(void* storage, std::size_t capacity);
void KHZ_ArchiveProbeProfileRestore(const void* storage, std::size_t capacity);
bool KHZ_ArchiveProbeProfileMatches(const void* storage, std::size_t capacity);
