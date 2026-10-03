// STAGED ONLY: END khz_archive.cpp. Requires native_probe_profile.inl in its
// original profile TU and the explicit pre-SV_Shutdown abort hook. Dedicated,
// private benchmark codec fixture; never a world checkpoint/replay provider.
#include "native_probe.h"
#include <Windows.h>
#include <qcommon/sv_msg_write_mp.h>
#include <script/scr_vm.h>
#include <script/scr_variable.h>
#include <script/scr_main.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <type_traits>

extern hudelem_s g_dummyHudCurrent;
extern hudelem_s g_dummyHudCurrent_1;

namespace khz_archive_probe {
using namespace khz_archive;
constexpr unsigned EntityCapacity = ENTITYNUM_NONE;
constexpr unsigned MaximumSplitRecords = 260;
constexpr std::uint64_t FnvBasis = 14695981039346656037ULL;
struct Digest {
    std::uint64_t value = FnvBasis, bytes = 0;
    void add(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) { value = (value ^ p[i]) * 1099511628211ULL; }
        bytes += size;
    }
};
struct Decoded {
    cachedSnapshot_t frame;
    cachedClient_s clients[NativeClientCapacity];
    archivedEntity_s entities[EntityCapacity];
};
#define KHZ_ARCHIVE_PROBE_CODEC_ARRAYS(X) \
    X(cachedSnapshotFrames) X(cachedSnapshotEntities) X(cachedSnapshotClients) \
    X(archivedSnapshotFrames) X(archivedSnapshotBuffer)
#define KHZ_ARCHIVE_PROBE_CODEC_SCALARS(X) \
    X(time) X(nextCachedSnapshotFrames) X(nextCachedSnapshotEntities) \
    X(nextCachedSnapshotClients) X(nextArchivedSnapshotFrames) X(nextArchivedSnapshotBuffer)
struct Image {
#define KHZ_ARCHIVE_PROBE_ARRAY_MEMBER(name) decltype(svs.name) name;
    KHZ_ARCHIVE_PROBE_CODEC_ARRAYS(KHZ_ARCHIVE_PROBE_ARRAY_MEMBER)
#undef KHZ_ARCHIVE_PROBE_ARRAY_MEMBER
#define KHZ_ARCHIVE_PROBE_SCALAR_MEMBER(name) decltype(svs.name) name;
    KHZ_ARCHIVE_PROBE_CODEC_SCALARS(KHZ_ARCHIVE_PROBE_SCALAR_MEMBER)
#undef KHZ_ARCHIVE_PROBE_SCALAR_MEMBER
    serverStaticHeader_t header;
    int headerValid;
    msg_t message;
    decltype(tempServerMsgBuf) temporary;
    bool archiving;
    State policy;
    netFieldOrderInfo_t fieldOrder;
    hudelem_s dummy, dummyLocal;
    std::uint64_t session, logicalTick;
    int serverId, levelTime, levelPreviousTime;
    void* entities;
    int entitySize, entityCount;
    std::size_t profileBytes;
    Digest original, world;
    // Own work buffers follow the saved fields. They are not native state and
    // are never restored to the engine or initialized through a giant temporary.
    Decoded expected;
    unsigned char wire[MessageCapacity], padding[MessageCapacity];
};
static_assert(std::is_trivially_copyable_v<Image>);
struct Result {
    bool completed, passed, cleanup, codecExecuted, aborted;
    bool fullDelta, liveOracle, logicalDelay, overflow, phantom;
    bool descriptorHeadroom, clientHeadroom, split, byteEviction, worldUnchanged;
    unsigned fullRecords, deltaRecords, decodes, comparisons, floatComparisons;
    unsigned logicalCases, splitRecords, reusedId;
    std::size_t allocationBytes, profileBytes;
    std::uint64_t session, tick, originalHash, restoredHash, hashedBytes;
    int serverId, nativeTime, clients, entities;
    char reason[160];
};
Image* image;
bool backupReady;
Result result;
KHZ_ArchiveProbeBoundary boundary;
char json[4096];

bool fail(const char* reason) {
    if (!result.reason[0]) std::snprintf(result.reason, sizeof(result.reason), "%s", reason);
    return false;
}
void* profile(Image* value) { return static_cast<void*>(value + 1); }
const void* profile(const Image* value) { return static_cast<const void*>(value + 1); }
void* profileScratch(Image* value) {
    return static_cast<unsigned char*>(profile(value)) + value->profileBytes;
}
const void* profileScratch(const Image* value) {
    return static_cast<const unsigned char*>(profile(value)) + value->profileBytes;
}
bool sameLifetime(const Image& value) {
    return Sys_IsMainThread() && sv.state == SS_GAME && svs.initialized &&
        KHZ_CurrentServerSession() == value.session && sv_serverId_value == value.serverId &&
        KHZ_ServerTickNumber() == value.logicalTick && sv.gentities == value.entities &&
        sv.gentitySize == value.entitySize && sv.num_entities == value.entityCount &&
        level.time == value.levelTime && level.previousTime == value.levelPreviousTime;
}
Digest worldDigest() {
    Digest digest;
    // Read-only observations of the actual data read by the native encoder;
    // not a full-world serializer or restoration image.
    digest.add(sv.gentities, std::size_t(sv.num_entities) * sv.gentitySize);
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        const auto* cs = G_GetClientState(i);
        const auto* ps = G_GetPlayerState(i);
        digest.add(cs, sizeof(clientState_s)); digest.add(ps, sizeof(playerState_s));
    }
    digest.add(&level.time, sizeof(level.time));
    digest.add(&level.previousTime, sizeof(level.previousTime));
    return digest;
}
Digest codecDigest(const Image* saved, bool native) {
    Digest digest;
    if (native) {
#define KHZ_ARCHIVE_PROBE_HASH_ARRAY(name) digest.add(&svs.name, sizeof(svs.name));
        KHZ_ARCHIVE_PROBE_CODEC_ARRAYS(KHZ_ARCHIVE_PROBE_HASH_ARRAY)
#undef KHZ_ARCHIVE_PROBE_HASH_ARRAY
#define KHZ_ARCHIVE_PROBE_HASH_SCALAR(name) digest.add(&svs.name, sizeof(svs.name));
        KHZ_ARCHIVE_PROBE_CODEC_SCALARS(KHZ_ARCHIVE_PROBE_HASH_SCALAR)
#undef KHZ_ARCHIVE_PROBE_HASH_SCALAR
        digest.add(&svsHeader, sizeof(svsHeader)); digest.add(&svsHeaderValid, sizeof(svsHeaderValid));
        digest.add(&g_archiveMsg, sizeof(g_archiveMsg)); digest.add(tempServerMsgBuf, sizeof(tempServerMsgBuf));
        digest.add(&g_archivingSnapshot, sizeof(g_archivingSnapshot));
        digest.add(&archivePolicy, sizeof(archivePolicy)); digest.add(&orderInfo, sizeof(orderInfo));
        digest.add(&g_dummyHudCurrent, sizeof(g_dummyHudCurrent));
        digest.add(&g_dummyHudCurrent_1, sizeof(g_dummyHudCurrent_1));
    }
    else {
#define KHZ_ARCHIVE_PROBE_HASH_IMAGE_ARRAY(name) digest.add(&saved->name, sizeof(saved->name));
        KHZ_ARCHIVE_PROBE_CODEC_ARRAYS(KHZ_ARCHIVE_PROBE_HASH_IMAGE_ARRAY)
#undef KHZ_ARCHIVE_PROBE_HASH_IMAGE_ARRAY
#define KHZ_ARCHIVE_PROBE_HASH_IMAGE_SCALAR(name) digest.add(&saved->name, sizeof(saved->name));
        KHZ_ARCHIVE_PROBE_CODEC_SCALARS(KHZ_ARCHIVE_PROBE_HASH_IMAGE_SCALAR)
#undef KHZ_ARCHIVE_PROBE_HASH_IMAGE_SCALAR
        digest.add(&saved->header, sizeof(saved->header)); digest.add(&saved->headerValid, sizeof(saved->headerValid));
        digest.add(&saved->message, sizeof(saved->message)); digest.add(saved->temporary, sizeof(saved->temporary));
        digest.add(&saved->archiving, sizeof(saved->archiving));
        digest.add(&saved->policy, sizeof(saved->policy)); digest.add(&saved->fieldOrder, sizeof(saved->fieldOrder));
        digest.add(&saved->dummy, sizeof(saved->dummy)); digest.add(&saved->dummyLocal, sizeof(saved->dummyLocal));
    }
    // The original profile fragment initializes its own padding consistently.
    // The post-restore digest uses a fresh capture of actual native fields.
    digest.add(native ? profileScratch(saved) : profile(saved), saved->profileBytes);
    return digest;
}
bool exactMatches(const Image& saved) {
#define KHZ_ARCHIVE_PROBE_MATCH_ARRAY(name) if (std::memcmp(&svs.name, &saved.name, sizeof(svs.name))) return false;
    KHZ_ARCHIVE_PROBE_CODEC_ARRAYS(KHZ_ARCHIVE_PROBE_MATCH_ARRAY)
#undef KHZ_ARCHIVE_PROBE_MATCH_ARRAY
#define KHZ_ARCHIVE_PROBE_MATCH_SCALAR(name) if (std::memcmp(&svs.name, &saved.name, sizeof(svs.name))) return false;
    KHZ_ARCHIVE_PROBE_CODEC_SCALARS(KHZ_ARCHIVE_PROBE_MATCH_SCALAR)
#undef KHZ_ARCHIVE_PROBE_MATCH_SCALAR
    return !std::memcmp(&svsHeader, &saved.header, sizeof(svsHeader)) && svsHeaderValid == saved.headerValid &&
        !std::memcmp(&g_archiveMsg, &saved.message, sizeof(g_archiveMsg)) &&
        !std::memcmp(tempServerMsgBuf, saved.temporary, sizeof(tempServerMsgBuf)) &&
        g_archivingSnapshot == saved.archiving && !std::memcmp(&archivePolicy, &saved.policy, sizeof(archivePolicy)) &&
        !std::memcmp(&orderInfo, &saved.fieldOrder, sizeof(orderInfo)) &&
        !std::memcmp(&g_dummyHudCurrent, &saved.dummy, sizeof(g_dummyHudCurrent)) &&
        !std::memcmp(&g_dummyHudCurrent_1, &saved.dummyLocal, sizeof(g_dummyHudCurrent_1)) &&
        KHZ_ArchiveProbeProfileMatches(profile(&saved), saved.profileBytes);
}
bool capture() {
    const auto bytes = KHZ_ArchiveProbeProfileBytes();
    if (!bytes || bytes > 1024 * 1024 || sizeof(Image) > SIZE_MAX - 2 * bytes) return fail("profile_image_extent_invalid");
    auto* value = static_cast<Image*>(HeapAlloc(GetProcessHeap(), 0, sizeof(Image) + 2 * bytes));
    if (!value) return fail("codec_backup_allocation_failed");
    image = value; backupReady = false; // abort can free an incomplete read-only capture
    // No engine mutation has occurred yet. Failure frees this exact allocation.
    value->profileBytes = bytes;
    if (!KHZ_ArchiveProbeProfileCapture(profile(value), bytes)) {
        image = nullptr; HeapFree(GetProcessHeap(), 0, value); return fail("profile_capture_rejected");
    }
#define KHZ_ARCHIVE_PROBE_CAPTURE_ARRAY(name) std::memcpy(&value->name, &svs.name, sizeof(svs.name));
    KHZ_ARCHIVE_PROBE_CODEC_ARRAYS(KHZ_ARCHIVE_PROBE_CAPTURE_ARRAY)
#undef KHZ_ARCHIVE_PROBE_CAPTURE_ARRAY
#define KHZ_ARCHIVE_PROBE_CAPTURE_SCALAR(name) std::memcpy(&value->name, &svs.name, sizeof(svs.name));
    KHZ_ARCHIVE_PROBE_CODEC_SCALARS(KHZ_ARCHIVE_PROBE_CAPTURE_SCALAR)
#undef KHZ_ARCHIVE_PROBE_CAPTURE_SCALAR
    std::memcpy(&value->header, &svsHeader, sizeof(svsHeader)); value->headerValid = svsHeaderValid;
    std::memcpy(&value->message, &g_archiveMsg, sizeof(g_archiveMsg));
    std::memcpy(value->temporary, tempServerMsgBuf, sizeof(tempServerMsgBuf));
    value->archiving = g_archivingSnapshot;
    std::memcpy(&value->policy, &archivePolicy, sizeof(archivePolicy));
    std::memcpy(&value->fieldOrder, &orderInfo, sizeof(orderInfo));
    std::memcpy(&value->dummy, &g_dummyHudCurrent, sizeof(g_dummyHudCurrent));
    std::memcpy(&value->dummyLocal, &g_dummyHudCurrent_1, sizeof(g_dummyHudCurrent_1));
    value->session = KHZ_CurrentServerSession(); value->logicalTick = KHZ_ServerTickNumber();
    value->serverId = sv_serverId_value; value->levelTime = level.time; value->levelPreviousTime = level.previousTime;
    value->entities = sv.gentities; value->entitySize = sv.gentitySize; value->entityCount = sv.num_entities;
    value->world = worldDigest();
    std::memset(&value->expected, 0, sizeof(value->expected));
    std::memset(value->wire, 0, sizeof(value->wire)); std::memset(value->padding, 0, sizeof(value->padding));
    value->original = codecDigest(value, false);
    if (!exactMatches(*value) || !sameLifetime(*value)) {
        image = nullptr; HeapFree(GetProcessHeap(), 0, value); return fail("codec_capture_source_changed");
    }
    result.allocationBytes = sizeof(Image) + 2 * bytes + sizeof(Decoded); result.profileBytes = bytes;
    result.originalHash = value->original.value; result.hashedBytes = value->original.bytes;
    backupReady = true; // complete validated original image before any mutation
    return true;
}
bool restoreAndFree() {
    if (!image) return false;
    auto* saved = image;
    if (!backupReady) {
        // Unexpected native error during capture, before any codec write.
        image = nullptr; HeapFree(GetProcessHeap(), 0, saved); return true;
    }
    if (!sameLifetime(*saved)) {
        // A lifetime release before the required abort hook is an integration
        // violation. Never write old bytes back into a different map/world.
        fail("codec_abort_hook_did_not_precede_native_lifetime_release");
        image = nullptr; backupReady = false; HeapFree(GetProcessHeap(), 0, saved); return false;
    }
    const auto world = worldDigest();
    result.worldUnchanged = world.value == saved->world.value && world.bytes == saved->world.bytes;
#define KHZ_ARCHIVE_PROBE_RESTORE_ARRAY(name) std::memcpy(&svs.name, &saved->name, sizeof(svs.name));
    KHZ_ARCHIVE_PROBE_CODEC_ARRAYS(KHZ_ARCHIVE_PROBE_RESTORE_ARRAY)
#undef KHZ_ARCHIVE_PROBE_RESTORE_ARRAY
#define KHZ_ARCHIVE_PROBE_RESTORE_SCALAR(name) std::memcpy(&svs.name, &saved->name, sizeof(svs.name));
    KHZ_ARCHIVE_PROBE_CODEC_SCALARS(KHZ_ARCHIVE_PROBE_RESTORE_SCALAR)
#undef KHZ_ARCHIVE_PROBE_RESTORE_SCALAR
    std::memcpy(&svsHeader, &saved->header, sizeof(svsHeader)); svsHeaderValid = saved->headerValid;
    std::memcpy(&g_archiveMsg, &saved->message, sizeof(g_archiveMsg));
    std::memcpy(tempServerMsgBuf, saved->temporary, sizeof(tempServerMsgBuf));
    g_archivingSnapshot = saved->archiving;
    std::memcpy(&archivePolicy, &saved->policy, sizeof(archivePolicy));
    std::memcpy(&orderInfo, &saved->fieldOrder, sizeof(orderInfo));
    std::memcpy(&g_dummyHudCurrent, &saved->dummy, sizeof(g_dummyHudCurrent));
    std::memcpy(&g_dummyHudCurrent_1, &saved->dummyLocal, sizeof(g_dummyHudCurrent_1));
    KHZ_ArchiveProbeProfileRestore(profile(saved), saved->profileBytes);
    const bool profileRead = KHZ_ArchiveProbeProfileCapture(profileScratch(saved), saved->profileBytes);
    const auto restored = codecDigest(saved, true);
    result.restoredHash = restored.value;
    const bool same = profileRead && exactMatches(*saved) && restored.value == saved->original.value &&
        restored.bytes == saved->original.bytes && result.worldUnchanged;
    // Allocation is released before ordinary SV_Shutdown/FinalMessage on abort.
    image = nullptr; backupReady = false; HeapFree(GetProcessHeap(), 0, saved);
    if (!same) fail("codec_exact_scope_or_observed_world_cleanup_failed");
    return same;
}
bool entry() {
#if !defined(KISAK_DEDICATED) || !defined(COD4X1KHZ) || defined(TRACY_ENABLE)
    return fail("requires_dedicated_custom_non_tracy_build");
#else
    if (!Sys_IsMainThread() || image || !boundary || !Dvar_GetBool("khz_benchmark") ||
        !Dvar_GetBool("khz_test_controls") || !com_dedicated || !com_dedicated->current.integer ||
        !com_sv_running || !com_sv_running->current.enabled || sv.state != SS_GAME ||
        sv.inFrame || com_inServerFrame || sv.restarting || sv.killServer || !svs.initialized ||
        svsHeaderValid || g_archivingSnapshot || archivePolicy.attempt.active ||
        !archivePolicy.indexValid || !archivePolicy.epoch ||
        archivePolicy.indexedFrame != svs.nextArchivedSnapshotFrames ||
        archivePolicy.indexedByte != svs.nextArchivedSnapshotBuffer || !Sys_IsDatabaseReady() ||
        !sv.gentities || sv.gentitySize <= 0 || sv.num_entities <= 0 || sv.num_entities > ENTITYNUM_NONE ||
        !sv_maxclients || sv_maxclients->current.integer <= 0 || sv_maxclients->current.integer > 16 ||
        svs.time < 0 || svs.time > INT_MAX - 20000 || !KHZ_CurrentServerSession())
        return fail("native_completed_owner_map_codec_boundary_invalid");
    if (!scrVarPub.bInited || !scrVarPub.programBuffer || scrVarPub.evaluate || scrVarPub.error_message ||
        scrVmPub.function_count || scrVmPub.inparamcount || scrVmPub.outparamcount ||
        scrVmPub.breakpointOutparamcount || scrVmPub.terminal_error || scrVmPub.debugCode ||
        scrVmPub.top != scrVmPub.stack || scrVmPub.function_frame != scrVmPub.function_frame_start)
        return fail("native_vm_not_idle");
    // These sources can otherwise synchronously emit external output or read
    // client prediction during a deliberately malformed/nonarchive message.
    if (Dvar_GetBool("sv_debugPacketContents") || Dvar_GetBool("sv_debugPlayerstate") ||
        Dvar_GetBool("msg_hudelemspew") || Dvar_GetBool("msg_dumpEnts") || Dvar_GetInt("cl_shownet"))
        return fail("native_codec_debug_output_enabled");
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        const auto& client = svs.clients[i];
        if (client.downloading || client.download || client.downloadingWWW ||
            client.header.netchan.unsentFragments || client.header.netchan.fragmentLength)
            return fail("native_download_or_fragment_active");
    }
    if (!boundary(result.reason, sizeof(result.reason)) || result.reason[0])
        return fail("source_owned_external_boundary_rejected");
    // Initialized before capture: never copy/reset a once_flag or Huffman tree.
    msg_t initialized;
    unsigned char byte = 0;
    MSG_Init(&initialized, &byte, 1);
    return true;
#endif
}
void freshBranch() {
    // Explicit temporary codec-only branch. World/level/script time never moves.
    svs.time = image->time;
    svs.nextCachedSnapshotFrames = svs.nextCachedSnapshotEntities = svs.nextCachedSnapshotClients = 0;
    svs.nextArchivedSnapshotFrames = svs.nextArchivedSnapshotBuffer = 0;
    for (auto& frame : svs.cachedSnapshotFrames) { std::memset(&frame, 0, sizeof(frame)); frame.archivedFrame = -1; }
    std::memset(svs.archivedSnapshotFrames, 0, sizeof(svs.archivedSnapshotFrames));
    archivePolicy.reset(image->policy.epoch); // no map reset or lifetime increment
    svsHeaderValid = 0; g_archivingSnapshot = false;
}
void invalidateCache() {
    for (auto& frame : svs.cachedSnapshotFrames) frame.archivedFrame = -1;
    svs.nextCachedSnapshotFrames += CacheFrameCapacity;
}
bool encode(int time, int paddedSize = 0, bool forceOverflow = false) {
    svs.time = time;
    SV_SetServerStaticHeader();
    if (!KHZ_ArchiveSampleDue()) { SV_GetServerStaticHeader(); return fail("fixture_sample_not_due"); }
    g_archivingSnapshot = true;
    SV_DisablePacketData();
    MSG_Init(&g_archiveMsg, tempServerMsgBuf, MessageCapacity);
    if (forceOverflow) MSG_WriteData(&g_archiveMsg, image->padding, MessageCapacity - 1);
    result.codecExecuted = true;
    SV_ArchiveSnapshot(&g_archiveMsg); // actual unchanged loaded world and codec
    if (paddedSize) {
        if (paddedSize < g_archiveMsg.cursize || paddedSize > MessageCapacity) {
            SV_GetServerStaticHeader(); g_archivingSnapshot = false;
            return fail("native_encode_exceeds_constructed_padding_extent");
        }
        MSG_WriteData(&g_archiveMsg, image->padding, unsigned(paddedSize - g_archiveMsg.cursize));
    }
    const auto kind = archivePolicy.attempt.kind;
    const bool overflow = g_archiveMsg.overflowed != 0;
    SV_GetServerStaticHeader();
    g_archivingSnapshot = false;
    KHZ_ArchiveCompleteMessage(&g_archiveMsg);
    if (forceOverflow != overflow) return fail("native_message_overflow_outcome_unexpected");
    if (!overflow) {
        if (kind == Kind::Full) ++result.fullRecords;
        else if (kind == Kind::Delta) ++result.deltaRecords;
        else return fail("native_encode_kind_missing");
    }
    return true;
}
bool copyDecoded(int id, Decoded* output) {
    const int before = svs.nextCachedSnapshotFrames;
    auto* frame = SV_GetCachedSnapshotInternal(id);
    if (!frame || !KHZ_ArchiveCachedFrameMatches(frame) || frame->num_clients <= 0 ||
        frame->num_clients > NativeClientCapacity || frame->num_entities <= 0 ||
        frame->num_entities > int(EntityCapacity)) return fail("native_decoded_frame_invalid_or_empty");
    if (svs.nextCachedSnapshotFrames > before) result.decodes += unsigned(svs.nextCachedSnapshotFrames - before);
    output->frame = *frame;
    for (int i = 0; i < frame->num_clients; ++i)
        std::memcpy(&output->clients[i], &svs.cachedSnapshotClients[(frame->first_client + i) % CacheClientCapacity], sizeof(cachedClient_s));
    for (int i = 0; i < frame->num_entities; ++i)
        std::memcpy(&output->entities[i], &svs.cachedSnapshotEntities[(frame->first_entity + i) % 16384], sizeof(archivedEntity_s));
    return true;
}
template<class T> bool sameValue(const T& a, const T& b) {
    ++result.comparisons;
    return !std::memcmp(&a, &b, sizeof(T));
}
bool fields(const void* a, const void* b, const NetField* values, unsigned count, std::size_t extent) {
    const auto* first = static_cast<const unsigned char*>(a);
    const auto* second = static_cast<const unsigned char*>(b);
    for (unsigned i = 0; i < count; ++i) {
        if (values[i].offset > extent || extent - values[i].offset < sizeof(std::uint32_t))
            return fail("native_field_offset_exceeds_serialized_type");
        ++result.comparisons;
        if (values[i].bits == 0 || values[i].bits == -88 || values[i].bits == -87 ||
            values[i].bits == -100 || values[i].bits == -90 || values[i].bits == -91 || values[i].bits == -92)
            ++result.floatComparisons;
        if (std::memcmp(first + values[i].offset, second + values[i].offset, 4))
            return fail("native_serialized_field_roundtrip_differs");
    }
    return true;
}
bool player(const playerState_s& a, const playerState_s& b) {
    if (!fields(&a, &b, playerStateFields, unsigned(sizeof(playerStateFields) / sizeof(playerStateFields[0])), sizeof(a)) ||
        !sameValue(a.stats, b.stats) || !sameValue(a.ammo, b.ammo) || !sameValue(a.ammoclip, b.ammoclip) ||
        !sameValue(a.weaponmodels, b.weaponmodels)) return fail("native_player_supplement_roundtrip_differs");
    for (unsigned i = 0; i < sizeof(a.objective) / sizeof(a.objective[0]); ++i) {
        const auto& x = a.objective[i]; const auto& y = b.objective[i];
        if (!sameValue(x.state, y.state) || !sameValue(x.origin, y.origin) || !sameValue(x.icon, y.icon) ||
            !sameValue(x.entNum, y.entNum) || !sameValue(x.teamNum, y.teamNum)) return fail("native_objective_roundtrip_differs");
    }
    for (unsigned i = 0; i < 31; ++i) {
        if (!fields(&a.hud.archival[i], &b.hud.archival[i], hudElemFields, 40, sizeof(hudelem_s)) ||
            !fields(&a.hud.current[i], &b.hud.current[i], hudElemFields, 40, sizeof(hudelem_s))) return false;
    }
    return true;
}
bool client(const cachedClient_s& a, const cachedClient_s& b) {
    return sameValue(a.cs.clientIndex, b.cs.clientIndex) &&
        fields(&a.cs, &b.cs, clientStateFields, 24, sizeof(a.cs)) &&
        sameValue(a.playerStateExists, b.playerStateExists) &&
        (!a.playerStateExists || player(a.ps, b.ps));
}
bool compare(const Decoded& expected, const Decoded& actual) {
    if (!sameValue(expected.frame.num_clients, actual.frame.num_clients) ||
        !sameValue(expected.frame.num_entities, actual.frame.num_entities)) return fail("native_serialized_row_counts_differ");
    for (int i = 0; i < expected.frame.num_clients; ++i)
        if (!client(expected.clients[i], actual.clients[i])) return fail("native_client_roundtrip_differs");
    for (int i = 0; i < expected.frame.num_entities; ++i)
        if (!sameValue(expected.entities[i].s.number, actual.entities[i].s.number) ||
            !fields(&expected.entities[i], &actual.entities[i], archivedEntityFields, 69, sizeof(archivedEntity_s))) return false;
    return true;
}
// One bounded additional decode buffer, allocated as part of an exact separate
// HeapAlloc before scope mutation. Not a giant automatic decoded-frame object.
Decoded* decoded;
bool verifyLiveOracle() {
    bool ok = true;
    SV_SetServerStaticHeader();
    unsigned connected = 0, linkedEligible = 0;
    for (int i = 0; i < svsHeader.maxclients; ++i)
        if (svsHeader.clients[i].header.state >= CS_CONNECTED) ++connected;
    for (int i = 0; i < svsHeader.num_entities; ++i) {
        const auto* live = SV_GentityNumLocal(i);
        if (live->r.linked && (live->r.broadcastTime ||
            ((live->r.svFlags & 1) == 0 && ((live->r.svFlags & 0x18) != 0 || svsHeader.svEntities[i].numClusters))))
            ++linkedEligible;
    }
    if (connected != unsigned(image->expected.frame.num_clients) ||
        linkedEligible != unsigned(image->expected.frame.num_entities))
        ok = fail("native_archive_row_coverage_differs_from_loaded_sources");
    for (int i = 0; ok && i < image->expected.frame.num_clients; ++i) {
        const auto& expected = image->expected.clients[i];
        const int index = expected.cs.clientIndex;
        if (index < 0 || index >= svsHeader.maxclients || svsHeader.clients[index].header.state < CS_CONNECTED) {
            ok = fail("native_oracle_client_index_invalid"); break;
        }
        SnapshotInfo_s info{}; info.archived = true; info.clientNum = index;
        msg_t wire; MSG_Init(&wire, image->wire, sizeof(image->wire)); MSG_ClearLastReferencedEntity(&wire);
        playerState_s current;
        const int exists = GetFollowPlayerStateLocal(index, &current);
        MSG_WriteDeltaClient(&info, &wire, svs.time, nullptr, G_GetClientStateLocal(index), 1);
        if (exists) { MSG_WriteBit1(&wire); MSG_WriteDeltaPlayerstate(&info, &wire, svs.time, nullptr, &current); }
        else MSG_WriteBit0(&wire);
        if (wire.overflowed) { ok = fail("native_oracle_client_message_overflowed"); break; }
        msg_t read; MSG_InitReadOnly(&read, image->wire, wire.cursize); MSG_BeginReading(&read);
        std::memset(&decoded->clients[0], 0, sizeof(cachedClient_s));
        if (MSG_ReadBit(&read) != 1) { ok = fail("native_oracle_client_presence_missing"); break; }
        const auto number = MSG_ReadEntityIndex(&read, 6);
        MSG_ReadDeltaClient(&read, svs.time, nullptr, &decoded->clients[0].cs, number);
        decoded->clients[0].playerStateExists = MSG_ReadBit(&read);
        if (decoded->clients[0].playerStateExists)
            MSG_ReadDeltaPlayerstate(0, &read, svs.time, nullptr, &decoded->clients[0].ps, false);
        ok = !read.overflowed && client(expected, decoded->clients[0]);
        if (!ok) fail("native_loaded_client_serialization_oracle_differs");
    }
    for (int i = 0; ok && i < image->expected.frame.num_entities; ++i) {
        const auto& expected = image->expected.entities[i];
        const int index = expected.s.number;
        const auto* live = SV_GentityNumLocal(index);
        if (!live || !live->r.linked || !(live->r.broadcastTime ||
            ((live->r.svFlags & 1) == 0 && ((live->r.svFlags & 0x18) != 0 || svsHeader.svEntities[index].numClusters)))) {
            ok = fail("native_oracle_entity_not_archive_eligible"); break;
        }
        archivedEntity_s current{};
        std::memcpy(&current.s, &live->s, sizeof(current.s)); current.r.svFlags = live->r.svFlags;
        if (live->r.broadcastTime) current.r.svFlags |= 8;
        std::memcpy(current.r.clientMask, live->r.clientMask, sizeof(current.r.clientMask));
        std::memcpy(current.r.absmin, live->r.absmin, sizeof(current.r.absmin));
        std::memcpy(current.r.absmax, live->r.absmax, sizeof(current.r.absmax));
        SnapshotInfo_s info{}; info.archived = true; info.fromBaseline = true;
        msg_t wire; MSG_Init(&wire, image->wire, sizeof(image->wire)); MSG_ClearLastReferencedEntity(&wire);
        MSG_WriteDeltaArchivedEntity(&info, &wire, svs.time, &sv.svEntities[index].baseline, &current, 1);
        if (wire.overflowed) { ok = fail("native_oracle_entity_message_overflowed"); break; }
        msg_t read; MSG_InitReadOnly(&read, image->wire, wire.cursize); MSG_BeginReading(&read);
        const auto number = MSG_ReadEntityIndex(&read, 10);
        MSG_ReadDeltaArchivedEntity(&read, svs.time, &sv.svEntities[index].baseline, &decoded->entities[0], number);
        ok = !read.overflowed && sameValue(expected.s.number, decoded->entities[0].s.number) &&
            fields(&expected, &decoded->entities[0], archivedEntityFields, 69, sizeof(expected));
        if (!ok) fail("native_loaded_entity_serialization_oracle_differs");
    }
    SV_GetServerStaticHeader();
    return ok;
}
bool basic() {
    freshBranch();
    const int first = image->time;
    if (!encode(first) || archivePolicy.entries[0].kind != Kind::Full || !encode(first + 50) ||
        archivePolicy.entries[1].kind != Kind::Delta || archivePolicy.entries[1].baseId != 0)
        return fail("native_full_delta_selection_missing");
    invalidateCache();
    if (!copyDecoded(0, &image->expected) || !copyDecoded(1, decoded) ||
        image->expected.frame.time != first || decoded->frame.time != first + 50 ||
        image->expected.frame.usesDelta || decoded->frame.usesDelta != 1 || !compare(image->expected, *decoded))
        return fail("native_full_delta_roundtrip_failed");
    result.clients = image->expected.frame.num_clients; result.entities = image->expected.frame.num_entities;
    result.fullDelta = true;
    if (!verifyLiveOracle()) return false;
    result.liveOracle = true;
    // Actual production lookup through real decoded data, all50 ms remainders.
    for (int remainder = 0; remainder < 50; ++remainder) {
        svs.time = first + 100 + remainder;
        int delay = 50 + remainder;
        auto* frame = KHZ_ArchiveLookup(&delay);
        if (!frame || frame->time != first + 50 || delay != 50 + remainder)
            return fail("native_lookup_eroded_requested_delay");
        int oldest = 1000;
        frame = KHZ_ArchiveLookup(&oldest);
        if (!frame || frame->time != first || oldest != svs.time - first)
            return fail("native_lookup_oldest_clamp_incorrect");
        int current = 0;
        if (KHZ_ArchiveLookup(&current) || current) return fail("native_lookup_current_fallback_incorrect");
        result.logicalCases += 3;
    }
    result.logicalDelay = true;
    return true;
}
bool overflowAndReuse() {
    const int id = svs.nextArchivedSnapshotFrames, bytes = svs.nextArchivedSnapshotBuffer;
    const int nextFrame = svs.nextCachedSnapshotFrames, nextClient = svs.nextCachedSnapshotClients;
    const int nextEntity = svs.nextCachedSnapshotEntities;
    const int time = image->time + 1100; // all earlier full bases >1000 ms old
    if (!encode(time, 0, true)) return false;
    auto* phantom = &svs.cachedSnapshotFrames[nextFrame % CacheFrameCapacity];
    if (svs.nextArchivedSnapshotFrames != id || svs.nextArchivedSnapshotBuffer != bytes ||
        svs.nextCachedSnapshotFrames != nextFrame + 1 || svs.nextCachedSnapshotClients <= nextClient ||
        svs.nextCachedSnapshotEntities <= nextEntity || phantom->archivedFrame != id ||
        phantom->time != time || phantom->usesDelta || KHZ_ArchiveCommittedDescriptor(id) ||
        KHZ_ArchiveCachedFrameMatches(phantom)) return fail("native_overflow_phantom_or_cache_progress_invalid");
    SV_SetServerStaticHeader();
    const bool phantomBase = KHZ_ArchiveCanUseDeltaBase(phantom);
    SV_GetServerStaticHeader();
    if (phantomBase) return fail("native_overflow_phantom_admitted_as_full_base");
    result.overflow = true;
    if (!encode(time + 50) || archivePolicy.entries[id % FrameCapacity].kind != Kind::Full ||
        svs.nextArchivedSnapshotFrames != id + 1 || KHZ_ArchiveCachedFrameMatches(phantom))
        return fail("native_overflow_reused_id_did_not_reject_old_time");
    invalidateCache();
    if (!copyDecoded(id, decoded) || decoded->frame.time != time + 50 || !compare(image->expected, *decoded))
        return fail("native_overflow_reused_payload_differs");
    result.phantom = true; result.reusedId = unsigned(id);
    return true;
}
bool headroom() {
    // Recreate the unchanged-world two-record branch after overflow scenarios.
    freshBranch();
    if (!encode(image->time) || !encode(image->time + 50)) return false;
    auto base = svs.cachedSnapshotFrames[0];
    invalidateCache();
    // Ring range contains the physically next write slot as its oldest member.
    auto* alias = &svs.cachedSnapshotFrames[svs.nextCachedSnapshotFrames % CacheFrameCapacity];
    *alias = base;
    if (KHZ_ArchiveCachedFrameMatches(alias)) return fail("native_full_base_descriptor_alias_admitted");
    const int before = svs.nextCachedSnapshotFrames;
    if (!copyDecoded(1, decoded) || svs.nextCachedSnapshotFrames != before + 2 ||
        !compare(image->expected, *decoded)) return fail("native_descriptor_headroom_fresh_decode_failed");
    result.descriptorHeadroom = true;
    // Copy the actual full client rows, not mock PS data, to the eviction edge.
    auto* currentBase = SV_GetCachedSnapshotInternal(0);
    if (!currentBase || !KHZ_ArchiveCachedFrameMatches(currentBase)) return fail("native_client_edge_full_base_missing");
    base = *currentBase;
    // Own fixed expected client rows are identical decoded full payloads and
    // avoid aliasing during a circular move of the real cache range.
    for (int i = 0; i < base.num_clients; ++i)
        std::memcpy(&decoded->clients[i], &svs.cachedSnapshotClients[(base.first_client + i) % CacheClientCapacity], sizeof(cachedClient_s));
    invalidateCache();
    for (int i = 0; i < base.num_clients; ++i)
        std::memcpy(&svs.cachedSnapshotClients[i], &decoded->clients[i], sizeof(cachedClient_s));
    base.first_client = 0;
    svs.nextCachedSnapshotClients = CacheClientCapacity - NativeClientCapacity + 1;
    auto* edge = &svs.cachedSnapshotFrames[(svs.nextCachedSnapshotFrames - 1) % CacheFrameCapacity];
    *edge = base;
    if (KHZ_ArchiveCachedFrameMatches(edge)) return fail("native_full_base_64_row_eviction_edge_admitted");
    const int edgeBefore = svs.nextCachedSnapshotFrames;
    if (!copyDecoded(1, decoded) || svs.nextCachedSnapshotFrames != edgeBefore + 2 ||
        !compare(image->expected, *decoded)) return fail("native_64_row_headroom_fresh_decode_failed");
    result.clientHeadroom = true;
    return true;
}
bool splitAndEviction() {
    freshBranch();
    const int target = ByteCapacity - 64;
    unsigned samples = 0;
    cachedSnapshot_t originalFull{};
    while (svs.nextArchivedSnapshotBuffer < target && samples < MaximumSplitRecords) {
        const int remaining = target - svs.nextArchivedSnapshotBuffer;
        const int bytes = remaining < MessageCapacity ? remaining : MessageCapacity;
        if (!encode(image->time + int(samples) * SampleMsec, bytes)) return false;
        if (!samples) originalFull = svs.cachedSnapshotFrames[0];
        ++samples;
    }
    if (svs.nextArchivedSnapshotBuffer != target) return fail("native_split_ring_tail_construction_failed");
    const int id = svs.nextArchivedSnapshotFrames;
    if (!encode(image->time + int(samples) * SampleMsec)) return false;
    const auto& raw = svs.archivedSnapshotFrames[id % FrameCapacity];
    if (raw.start % ByteCapacity != ByteCapacity - 64 || raw.size <= 64)
        return fail("native_encoded_record_did_not_split_byte_ring");
    // Both recursive full-base and result must use the native raw decoder.
    invalidateCache();
    if (!copyDecoded(id, decoded) || !compare(image->expected, *decoded))
        return fail("native_split_read_serialized_payload_differs");
    result.split = true; result.splitRecords = samples + 1;
    auto* old = &svs.cachedSnapshotFrames[(svs.nextCachedSnapshotFrames - 1) % CacheFrameCapacity];
    *old = originalFull; // cache presence alone must never resurrect raw id0
    if (KHZ_ArchiveCommittedDescriptor(0) || KHZ_ArchiveCachedFrameMatches(old) || SV_GetCachedSnapshotInternal(0))
        return fail("native_byte_eviction_resurrected_cached_full_base");
    result.byteEviction = true;
    return true;
}
void escaped(char* output, std::size_t capacity, const char* input) {
    std::size_t count = 0;
    for (std::size_t i = 0; input[i] && count + 2 < capacity; ++i) {
        const auto ch = static_cast<unsigned char>(input[i]);
        if (ch == '"' || ch == '\\') { output[count++] = '\\'; output[count++] = char(ch); }
        else if (ch >= 32 && ch < 127) output[count++] = char(ch);
        else output[count++] = '?';
    }
    output[count] = 0;
}
}

bool KHZ_ArchiveProbeInstallBoundary(KHZ_ArchiveProbeBoundary value) {
    if (!Sys_IsMainThread() || khz_archive_probe::image || !value) return false;
    khz_archive_probe::boundary = value; return true;
}
bool KHZ_ArchiveCodecProbeActive() { return Sys_IsMainThread() && khz_archive_probe::image != nullptr; }
bool KHZ_ArchiveRunCodecProbe() {
    using namespace khz_archive_probe;
    if (!Sys_IsMainThread() || image) return false;
    std::memset(&result, 0, sizeof(result));
    if (!entry()) { result.completed = true; return false; }
    result.session = KHZ_CurrentServerSession(); result.tick = KHZ_ServerTickNumber();
    result.serverId = sv_serverId_value; result.nativeTime = svs.time;
    // Both exact bounded allocations are complete before the first codec write.
    decoded = static_cast<Decoded*>(HeapAlloc(GetProcessHeap(), 0, sizeof(Decoded)));
    if (!decoded) { fail("decoded_work_buffer_allocation_failed"); result.completed = true; return false; }
    if (!capture()) {
        HeapFree(GetProcessHeap(), 0, decoded); decoded = nullptr; result.completed = true; return false;
    }
    const bool success = basic() && overflowAndReuse() && headroom() && splitAndEviction();
    result.cleanup = restoreAndFree();
    HeapFree(GetProcessHeap(), 0, decoded); decoded = nullptr;
    result.completed = true;
    result.passed = success && result.cleanup && result.comparisons && result.floatComparisons &&
        result.decodes && result.fullRecords && result.deltaRecords;
    return result.passed;
}
void KHZ_ArchiveProbeAbortBeforeServerShutdown() {
    using namespace khz_archive_probe;
    if (!Sys_IsMainThread() || !image) return;
    result.aborted = true; result.passed = false;
    fail("unexpected_native_error_aborted_codec_fixture");
    result.cleanup = restoreAndFree();
    if (decoded) { HeapFree(GetProcessHeap(), 0, decoded); decoded = nullptr; }
    result.completed = true;
}
const char* KHZ_ArchiveCodecProbeJSON() {
    using namespace khz_archive_probe;
    if (!Sys_IsMainThread()) return "{}";
    char reason[320]; escaped(reason, sizeof(reason), result.reason);
    std::snprintf(json, sizeof(json),
        "{\"completed\":%s,\"passed\":%s,\"cleanup_complete\":%s,\"native_codec_executed\":%s,"
        "\"aborted\":%s,\"full_world_restore\":false,\"authoritative_replay\":false,"
        "\"session\":%llu,\"tick\":%llu,\"server_id\":%d,\"native_time\":%d,"
        "\"clients\":%d,\"entities\":%d,\"full_records\":%u,\"delta_records\":%u,"
        "\"native_decodes\":%u,\"serialized_comparisons\":%u,\"float_comparisons\":%u,"
        "\"full_delta\":%s,\"live_serialization_oracle\":%s,\"logical_delay\":%s,\"logical_cases\":%u,"
        "\"real_msg_overflow\":%s,\"phantom_rejected\":%s,\"reused_id\":%u,"
        "\"descriptor_headroom\":%s,\"client_64_row_headroom\":%s,\"split_byte_ring\":%s,"
        "\"split_records\":%u,\"byte_eviction\":%s,\"observed_world_unchanged\":%s,"
        "\"allocation_bytes\":%llu,\"profile_bytes\":%llu,\"diagnostic_fnv64_before\":\"%016llx\","
        "\"diagnostic_fnv64_after\":\"%016llx\",\"hashed_bytes\":%llu,\"reason\":\"%s\"}",
        result.completed ? "true" : "false", result.passed ? "true" : "false", result.cleanup ? "true" : "false",
        result.codecExecuted ? "true" : "false", result.aborted ? "true" : "false", result.session, result.tick,
        result.serverId, result.nativeTime, result.clients, result.entities, result.fullRecords, result.deltaRecords,
        result.decodes, result.comparisons, result.floatComparisons, result.fullDelta ? "true" : "false",
        result.liveOracle ? "true" : "false", result.logicalDelay ? "true" : "false", result.logicalCases,
        result.overflow ? "true" : "false", result.phantom ? "true" : "false", result.reusedId,
        result.descriptorHeadroom ? "true" : "false", result.clientHeadroom ? "true" : "false",
        result.split ? "true" : "false", result.splitRecords, result.byteEviction ? "true" : "false",
        result.worldUnchanged ? "true" : "false", static_cast<unsigned long long>(result.allocationBytes),
        static_cast<unsigned long long>(result.profileBytes), result.originalHash, result.restoredHash,
        result.hashedBytes, reason);
    return json;
}
#undef KHZ_ARCHIVE_PROBE_CODEC_ARRAYS
#undef KHZ_ARCHIVE_PROBE_CODEC_SCALARS
