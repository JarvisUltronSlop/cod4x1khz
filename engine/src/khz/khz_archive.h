#pragma once
// Source-owned bounded metadata for the stock historical snapshot codec.
#include <archive_history.hpp>
#include <universal/q_shared.h>
#include <server_mp/server_mp.h>

struct KHZ_ArchiveSidecarImage {
    khz_archive::State policy;
    std::array<archivedSnapshot_s, khz_archive::FrameCapacity> descriptors;
    int nextFrame;
    int nextByte;
    int nativeTime;
};

void KHZ_ArchiveResetPolicy();
void KHZ_ArchiveShutdownComplete();
bool KHZ_ArchiveSampleDue();
bool KHZ_ArchiveCanUseDeltaBase(const cachedSnapshot_t* base);
void KHZ_ArchiveEncodedDelta(const cachedSnapshot_t* base);
void KHZ_ArchiveEncodedFull();
void KHZ_ArchiveCompleteMessage(const msg_t* message);
bool KHZ_ArchiveCommittedDescriptor(int id);
bool KHZ_ArchiveCachedFrameMatches(const cachedSnapshot_t* frame);
bool KHZ_ArchiveDecodedTimeMatches(int id, int time);
bool KHZ_ArchiveDecodedKindMatches(int id, bool usesDelta);
bool KHZ_ArchiveDeltaReferenceMatches(int id, int baseId);
cachedSnapshot_t* KHZ_ArchiveLookup(int* logicalDelay);
char KHZ_ArchiveClientPosition(int client, int gametime, float* pos);
bool KHZ_ArchiveCopySidecar(KHZ_ArchiveSidecarImage* output);
bool KHZ_ArchiveValidateSidecar(const KHZ_ArchiveSidecarImage& image, int savedNativeTime);
bool KHZ_ArchiveSidecarMatchesNativeImage(const KHZ_ArchiveSidecarImage& image,
    const serverStatic_t& savedNativeImage);
void KHZ_ArchiveRestoreSidecar(const KHZ_ArchiveSidecarImage& image);
const char* KHZ_ArchiveDiagnosticsJSON();

