// Stock 20 Hz archive, independent of the custom 1 kHz gameplay/network clocks.
#include <universal/q_shared.h>
#include "khz_archive.h"
#include <khz/khz_runtime.h>
#include <game_mp/g_public_mp.h>
#include <qcommon/threads.h>
#include <cstring>
#include <cstdint>
#include <cstdio>

extern msg_t g_archiveMsg; // Actual native message; defined in sv_snapshot_mp.cpp.

namespace {
using namespace khz_archive;
State archivePolicy; // Trivial static BSS; no large aggregate startup temporary.

const Entry* descriptor(int id) {
    if (id < 0 || id >= svs.nextArchivedSnapshotFrames) return nullptr;
    const auto& native = svs.archivedSnapshotFrames[unsigned(id) % FrameCapacity];
    return archivePolicy.committed(id, svs.nextArchivedSnapshotFrames,
        svs.nextArchivedSnapshotBuffer, native.start, native.size);
}
bool available(const Entry& entry) {
    const auto* actual = descriptor(entry.id);
    if (!actual || actual != &entry) return false;
    if (entry.kind == Kind::Full) return true;
    const auto* base = descriptor(entry.baseId);
    // Full bases only: never permit a chain, future frame, self-reference or
    // cached hit to bypass an evicted base. Cache presence must not change the
    // public historical availability policy.
    return base && base->kind == Kind::Full && base->id < entry.id &&
        base->nativeTime == entry.baseTime && base->nativeTime <= entry.nativeTime &&
        std::int64_t(entry.nativeTime) - base->nativeTime <= FullBaseMsec;
}
const Entry* descriptor(const KHZ_ArchiveSidecarImage& image, int id) {
    if (id < 0 || id >= image.nextFrame) return nullptr;
    const auto& native = image.descriptors[unsigned(id) % FrameCapacity];
    return image.policy.committed(id, image.nextFrame, image.nextByte, native.start, native.size);
}
bool available(const KHZ_ArchiveSidecarImage& image, const Entry& entry) {
    const auto* actual = descriptor(image, entry.id);
    if (!actual || actual != &entry) return false;
    if (entry.kind == Kind::Full) return true;
    const auto* base = descriptor(image, entry.baseId);
    return base && base->kind == Kind::Full && base->id < entry.id &&
        base->nativeTime == entry.baseTime && base->nativeTime <= entry.nativeTime &&
        std::int64_t(entry.nativeTime) - base->nativeTime <= FullBaseMsec;
}
bool cacheStorageAndRanges(const cachedSnapshot_t* frame, int nextEntities, int nextClients) {
    if (!frame) return false;
    const auto pointer = reinterpret_cast<std::uintptr_t>(frame);
    const auto first = reinterpret_cast<std::uintptr_t>(svs.cachedSnapshotFrames);
    if (pointer < first || pointer - first >= sizeof(svs.cachedSnapshotFrames) ||
        (pointer - first) % sizeof(*frame)) return false;
    return frame->num_entities >= 0 && frame->num_entities < 16384 &&
        frame->num_clients >= 0 && frame->num_clients <= NativeClientCapacity &&
        frame->first_entity >= 0 && frame->first_client >= 0 &&
        std::int64_t(frame->first_entity) >= std::int64_t(nextEntities) - 16384 &&
        std::int64_t(frame->first_client) >= std::int64_t(nextClients) - 4096 &&
        std::int64_t(frame->first_entity) + frame->num_entities <= nextEntities &&
        std::int64_t(frame->first_client) + frame->num_clients <= nextClients;
}
cachedSnapshot_t* readCommitted(int id) {
    const auto* expected = descriptor(id);
    if (!expected || !available(*expected)) return nullptr;
    auto* value = SV_GetCachedSnapshotInternal(id);
    // Unexpected decoder disagreement is corruption, not a "no history" alias.
    if (value && !KHZ_ArchiveCachedFrameMatches(value))
        Com_Error(ERR_DROP, "Archive decoded cache identity differs from committed descriptor");
    return value;
}
bool copyPosition(int client, const cachedSnapshot_t* frame, float* position) {
    for (int index = 0; index < frame->num_clients; ++index) {
        const auto& value = svs.cachedSnapshotClients[
            unsigned(std::int64_t(frame->first_client) + index) % CacheClientCapacity];
        if (value.cs.clientIndex != client) continue;
        if (!value.playerStateExists || value.ps.stats[STAT_HEALTH] <= 0 ||
            !(value.ps.otherFlags & 4) || (value.ps.otherFlags & 2)) return false;
        std::memcpy(position, value.ps.origin, 3 * sizeof(float));
        return true;
    }
    return false;
}
}

void KHZ_ArchiveResetPolicy() {
    iassert(Sys_IsMainThread() && !svsHeaderValid && !g_archivingSnapshot);
    // This is a completed owner lifecycle boundary after native counters reset,
    // not rollback. A stale abandoned error-path attempt may be invalidated
    // only after header/archiving writers have actually been closed.
    if (svs.nextArchivedSnapshotFrames || svs.nextArchivedSnapshotBuffer)
        Com_Error(ERR_DROP, "Archive lifecycle reset must follow native archive counter reset");
    const auto previous = archivePolicy.epoch;
    if (previous == UINT64_MAX) Com_Error(ERR_FATAL, "Archive lifetime epoch exhausted");
    archivePolicy.reset(previous + 1);
}
void KHZ_ArchiveShutdownComplete() {
    iassert(Sys_IsMainThread());
    if (sv.state != SS_DEAD || com_inServerFrame || svs.initialized ||
        svs.nextArchivedSnapshotFrames || svs.nextArchivedSnapshotBuffer)
        Com_Error(ERR_FATAL, "Archive shutdown invalidation requires completed original native shutdown");
    // Error longjmp may abandon the snapshot dispatch, but the original server
    // has now been synchronously shut down and its sv/svs storage cleared.
    // No live header/cache writer may resume. Do not import abandoned counters.
    svsHeaderValid = 0;
    g_archivingSnapshot = false;
    archivePolicy.attempt.active = false;
    archivePolicy.indexValid = false;
}
bool KHZ_ArchiveSampleDue() {
    iassert(Sys_IsMainThread() && svsHeaderValid);
    if (sv.state != SS_GAME) return false;
    // Payload time is native time. The captured sidecar uses a separate epoch
    // because svs.time is cumulative across map/fast-restart lifetimes.
    const auto result = archivePolicy.begin(svs.time, KHZ_ServerTickNumber(),
        svs.nextArchivedSnapshotFrames, svs.nextArchivedSnapshotBuffer);
    if (result == Due::NotDue) return false;
    if (result != Due::Started)
        Com_Error(ERR_DROP, "Archive scheduling requires a reset original native clock");
    return true;
}
bool KHZ_ArchiveCanUseDeltaBase(const cachedSnapshot_t* base) {
    iassert(Sys_IsMainThread() && svsHeaderValid);
    if (!cacheStorageAndRanges(base, svsHeader.nextCachedSnapshotEntities,
        svsHeader.nextCachedSnapshotClients) || base->usesDelta) return false;
    return archivePolicy.fullBase(descriptor(base->archivedFrame),
        base->archivedFrame, base->time);
}
void KHZ_ArchiveEncodedDelta(const cachedSnapshot_t* base) {
    if (!KHZ_ArchiveCanUseDeltaBase(base) ||
        !archivePolicy.encodedDelta(descriptor(base->archivedFrame), base->archivedFrame, base->time))
        Com_Error(ERR_DROP, "Archive delta encoder did not select a committed full base");
}
void KHZ_ArchiveEncodedFull() {
    iassert(Sys_IsMainThread() && svsHeaderValid);
    if (!archivePolicy.encodedFull())
        Com_Error(ERR_DROP, "Archive full encoder attempt is missing or repeated");
}
void KHZ_ArchiveCompleteMessage(const msg_t* message) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    if (!message || !archivePolicy.attempt.active)
        Com_Error(ERR_DROP, "Archive commit lacks its actual native encode attempt");
    if (message != &g_archiveMsg || message->data != tempServerMsgBuf ||
        message->readOnly || message->splitData || message->splitSize ||
        message->maxsize != MessageCapacity)
        Com_Error(ERR_DROP, "Archive commit requires its actual bounded native message storage");
    if (!validKind(archivePolicy.attempt.kind) ||
        archivePolicy.attempt.id != svs.nextArchivedSnapshotFrames ||
        archivePolicy.attempt.start != svs.nextArchivedSnapshotBuffer ||
        archivePolicy.attempt.nativeTime != svs.time ||
        message->cursize < 0 || message->cursize > MessageCapacity)
        Com_Error(ERR_DROP, "Archive actual encode tuple changed before completion");
    if (message->overflowed) {
        archivePolicy.abortOverflow();
        return; // Cache writes/counters naturally stand; no descriptor becomes committed.
    }
    const int id = svs.nextArchivedSnapshotFrames, start = svs.nextArchivedSnapshotBuffer;
    const int bytes = message->cursize;
    if (!archivePolicy.canCommit(bytes, id, start) ||
        archivePolicy.attempt.nativeTime != svs.time)
        Com_Error(ERR_DROP, "Archive message metadata changed before commit");
    const int offset = start % ByteCapacity;
    const int firstBytes = (bytes < ByteCapacity - offset) ? bytes : ByteCapacity - offset;
    // All preconditions precede byte/descriptor mutations. Same owner, no
    // callback or fallible operation occurs between this check and publication.
    std::memcpy(svs.archivedSnapshotBuffer + offset, message->data, firstBytes);
    if (bytes > firstBytes)
        std::memcpy(svs.archivedSnapshotBuffer, message->data + firstBytes, bytes - firstBytes);
    svs.archivedSnapshotFrames[unsigned(id) % FrameCapacity] = {start, bytes};
    if (!archivePolicy.commit(bytes, id, start))
        Com_Error(ERR_FATAL, "Archive checked commit invariant failed");
    svs.nextArchivedSnapshotBuffer = start + bytes;
    svs.nextArchivedSnapshotFrames = id + 1;
    if (!archivePolicy.refreshAvailability(svs.nextArchivedSnapshotFrames,
        svs.nextArchivedSnapshotBuffer, [](const Entry& entry) { return available(entry); }))
        Com_Error(ERR_FATAL, "Archive checked availability index invariant failed");
}
bool KHZ_ArchiveCommittedDescriptor(int id) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    const auto* entry = descriptor(id);
    return entry && available(*entry);
}
bool KHZ_ArchiveCachedFrameMatches(const cachedSnapshot_t* frame) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    if (!cacheStorageAndRanges(frame, svs.nextCachedSnapshotEntities,
        svs.nextCachedSnapshotClients)) return false;
    const int index = int(frame - svs.cachedSnapshotFrames);
    if (!decoderWriteHeadroom(index, svs.nextCachedSnapshotFrames,
        frame->first_client, frame->num_clients, svs.nextCachedSnapshotClients)) return false;
    const auto* entry = descriptor(frame->archivedFrame);
    return entry && available(*entry) && entry->nativeTime == frame->time &&
        (frame->usesDelta == 0 || frame->usesDelta == 1) &&
        bool(frame->usesDelta) == (entry->kind == Kind::Delta);
}
bool KHZ_ArchiveDecodedTimeMatches(int id, int time) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    const auto* entry = descriptor(id);
    return entry && available(*entry) && entry->nativeTime == time;
}
bool KHZ_ArchiveDecodedKindMatches(int id, bool usesDelta) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    const auto* entry = descriptor(id);
    return entry && available(*entry) && (entry->kind == Kind::Delta) == usesDelta;
}
bool KHZ_ArchiveDeltaReferenceMatches(int id, int baseId) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    const auto* entry = descriptor(id);
    return entry && entry->kind == Kind::Delta && entry->baseId == baseId && available(*entry);
}
cachedSnapshot_t* KHZ_ArchiveLookup(int* logicalDelay) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    iassert(SV_Loaded());
    if (!logicalDelay) return nullptr;
    const auto selected = archivePolicy.select(svs.time, *logicalDelay,
        svs.nextArchivedSnapshotFrames, svs.nextArchivedSnapshotBuffer);
    if (!selected.valid) Com_Error(ERR_DROP, "Archive timestamp/availability index differs from native owner");
    if (selected.id < 0) { *logicalDelay = selected.logicalDelay; return nullptr; }
    auto* result = readCommitted(selected.id);
    if (!result) Com_Error(ERR_DROP, "Committed archive selection failed native decoding");
    *logicalDelay = selected.logicalDelay;
    return result;
}
char KHZ_ArchiveClientPosition(int client, int gametime, float* pos) {
    iassert(Sys_IsMainThread() && !svsHeaderValid);
    if (!pos || client < 0 || client >= sv_maxclients->current.integer ||
        gametime <= 0 || gametime > svs.time ||
        std::int64_t(svs.time) - gametime > 400) return 0;
    playerState_s current; clientState_s state;
    if (!SV_GetCurrentClientInfo(client, &current, &state) ||
        current.stats[STAT_HEALTH] <= 0 || !(current.otherFlags & 4) || (current.otherFlags & 2)) return 0;
    float newer[3]; std::memcpy(newer, current.origin, sizeof(newer));
    int newerTime = svs.time;
    if (gametime == newerTime) { std::memcpy(pos, newer, sizeof(newer)); return 1; }
    const int frameFirst = (svs.nextArchivedSnapshotFrames > FrameCapacity)
        ? svs.nextArchivedSnapshotFrames - FrameCapacity : 0;
    if (!archivePolicy.indexValid || archivePolicy.indexedFrame != svs.nextArchivedSnapshotFrames ||
        archivePolicy.indexedByte != svs.nextArchivedSnapshotBuffer)
        Com_Error(ERR_DROP, "Legacy archive position lookup differs from native index");
    const int first = archivePolicy.lowerBoundTime(frameFirst, svs.nextArchivedSnapshotFrames,
        std::int64_t(svs.time) - 400 - SampleMsec);
    for (int id = svs.nextArchivedSnapshotFrames - 1; id >= first; --id) {
        const auto* entry = descriptor(id);
        if (!entry || !available(*entry) || entry->nativeTime >= newerTime) continue;
        // One older 50 ms sample may be needed to bracket a requested age400.
        if (std::int64_t(svs.time) - entry->nativeTime > 400 + SampleMsec) break;
        auto* frame = readCommitted(id);
        if (!frame) continue;
        float older[3];
        if (!copyPosition(client, frame, older)) return 0;
        const int olderTime = frame->time;
        if (olderTime <= gametime) {
            const double fraction = double(gametime - olderTime) / double(newerTime - olderTime);
            if (fraction < 0.0 || fraction > 1.0) return 0;
            for (unsigned axis = 0; axis != 3; ++axis)
                pos[axis] = float(older[axis] + fraction * (newer[axis] - older[axis]));
            return 1;
        }
        std::memcpy(newer, older, sizeof(newer)); newerTime = olderTime;
    }
    return 0;
}

bool KHZ_ArchiveCopySidecar(KHZ_ArchiveSidecarImage* output) {
    if (!Sys_IsMainThread() || !output) return false;
    if (archivePolicy.attempt.active || svsHeaderValid || g_archivingSnapshot || !archivePolicy.epoch ||
        !archivePolicy.indexValid || archivePolicy.indexedFrame != svs.nextArchivedSnapshotFrames ||
        archivePolicy.indexedByte != svs.nextArchivedSnapshotBuffer)
        return false;
    output->policy = archivePolicy;
    std::memcpy(output->descriptors.data(), svs.archivedSnapshotFrames, sizeof(svs.archivedSnapshotFrames));
    output->nextFrame = svs.nextArchivedSnapshotFrames;
    output->nextByte = svs.nextArchivedSnapshotBuffer;
    output->nativeTime = svs.time;
    return true;
}
bool KHZ_ArchiveValidateSidecar(const KHZ_ArchiveSidecarImage& image, int savedNativeTime) {
    if (!Sys_IsMainThread() || archivePolicy.attempt.active || svsHeaderValid || g_archivingSnapshot)
        return false;
    if (!image.policy.epoch || image.policy.epoch != archivePolicy.epoch ||
        image.policy.attempt.active || image.nativeTime != savedNativeTime ||
        image.nextFrame < 0 || image.nextByte < 0 || image.nativeTime < 0 ||
        image.policy.overflows > image.policy.attempts ||
        image.policy.attempts - image.policy.overflows != unsigned(image.nextFrame) ||
        !image.policy.indexValid || image.policy.indexedFrame != image.nextFrame ||
        image.policy.indexedByte != image.nextByte) return false;
    if (image.policy.clockStarted) {
        if (image.policy.lastOwnerTime < 0 || image.policy.lastOwnerTime > image.nativeTime ||
            image.policy.nextDueTime <= unsigned(image.policy.lastOwnerTime) ||
            image.policy.nextDueTime - unsigned(image.policy.lastOwnerTime) > SampleMsec) return false;
    }
    else if (image.nextFrame || image.nextByte || image.policy.attempts || image.policy.overflows ||
        image.policy.skippedDeadlines || image.policy.nextDueTime || image.policy.lastOwnerTime)
        return false;
    const int first = image.nextFrame > FrameCapacity ? image.nextFrame - FrameCapacity : 0;
    int previousTime = -1;
    std::int64_t previousEnd = -1;
    for (int id = first; id < image.nextFrame; ++id) {
        const auto& native = image.descriptors[unsigned(id) % FrameCapacity];
        const auto& entry = image.policy.entries[unsigned(id) % FrameCapacity];
        if (entry.epoch != image.policy.epoch || entry.id != id ||
            entry.nativeTime <= previousTime || entry.nativeTime > image.nativeTime ||
            entry.start != native.start || entry.size != native.size ||
            entry.size <= 0 || entry.size > MessageCapacity || entry.start < 0 ||
            std::int64_t(entry.start) + entry.size > image.nextByte ||
            (previousEnd >= 0 && entry.start != previousEnd) || !validKind(entry.kind) ||
            (entry.kind == Kind::Full && (entry.baseId != -1 || entry.baseTime != 0)) ||
            (entry.kind == Kind::Delta && (entry.baseId < 0 || entry.baseId >= id ||
                entry.baseTime < 0 || entry.baseTime > entry.nativeTime ||
                std::int64_t(entry.nativeTime) - entry.baseTime > FullBaseMsec))) return false;
        if (entry.kind == Kind::Delta && entry.baseId >= first) {
            const auto& base = image.policy.entries[unsigned(entry.baseId) % FrameCapacity];
            if (base.epoch != entry.epoch || base.id != entry.baseId || base.kind != Kind::Full ||
                base.nativeTime != entry.baseTime) return false;
        }
        previousTime = entry.nativeTime;
        previousEnd = std::int64_t(entry.start) + entry.size;
    }
    if (image.nextFrame && (!image.policy.clockStarted || previousTime > image.policy.lastOwnerTime)) return false;
    if (image.nextFrame && previousEnd != image.nextByte) return false;
    std::array<std::uint64_t, AvailabilityWords> expected;
    if (!image.policy.buildAvailability(expected, image.nextFrame, image.nextByte,
        [&](const Entry& entry) { return available(image, entry); }) ||
        expected != image.policy.availability) return false;
    // Original pointer/map/native-provider checks and actual pause/fence are
    // mandatory in the calling coordinator. This does not attest their presence.
    return true;
}
bool KHZ_ArchiveSidecarMatchesNativeImage(const KHZ_ArchiveSidecarImage& image,
    const serverStatic_t& savedNativeImage) {
    if (!Sys_IsMainThread()) return false;
    // Call with the actual captured Network part0 svs image, whose allocation
    // and capture lifetime were validated by its source-owned provider.
    return image.nextFrame == savedNativeImage.nextArchivedSnapshotFrames &&
        image.nextByte == savedNativeImage.nextArchivedSnapshotBuffer &&
        image.nativeTime == savedNativeImage.time &&
        std::memcmp(image.descriptors.data(), savedNativeImage.archivedSnapshotFrames,
            sizeof(savedNativeImage.archivedSnapshotFrames)) == 0;
}
void KHZ_ArchiveRestoreSidecar(const KHZ_ArchiveSidecarImage& image) {
    iassert(Sys_IsMainThread() && !svsHeaderValid && !g_archivingSnapshot && !archivePolicy.attempt.active);
    archivePolicy = image.policy;
    // Native svs descriptors/bytes/cache rings are restored by Network part0,
    // not by this metadata provider. Commit is infallible after ALL validation.
}
const char* KHZ_ArchiveDiagnosticsJSON() {
    if (!Sys_IsMainThread()) return "{}";
    static char result[768];
    std::snprintf(result, sizeof(result),
        "{\"sample_msec\":%d,\"epoch\":%llu,\"attempts\":%llu,\"overflows\":%llu,"
        "\"skipped_opportunities\":%llu,\"committed_frames\":%d,\"committed_bytes\":%d,"
        "\"attempt_active\":%s,\"index_valid\":%s,\"native_time\":%d,\"next_due_time\":%llu}",
        khz_archive::SampleMsec, archivePolicy.epoch, archivePolicy.attempts,
        archivePolicy.overflows, archivePolicy.skippedDeadlines, svs.nextArchivedSnapshotFrames,
        svs.nextArchivedSnapshotBuffer, archivePolicy.attempt.active ? "true" : "false",
        archivePolicy.indexValid ? "true" : "false", svs.time, archivePolicy.nextDueTime);
    return result;
}
