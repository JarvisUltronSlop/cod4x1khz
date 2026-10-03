// Isolated synthetic-state contracts of the actual selected adapter body.
// Native layouts are real; owner, error, current-PS and decoder callbacks are
// synthetic dependencies. This does not run the native codec or a loaded map.
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <script/scr_const.h>
#if defined(KHZ_PRODUCTION_ADAPTER_TEST)
#include <khz/khz_archive.h>
#else
#include "native_adapter.h"
#endif
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

serverStatic_t svs; // Original layout, static BSS; no giant startup temporary.
server_t sv;
serverStaticHeader_t svsHeader;
msg_t g_archiveMsg;
uint8_t tempServerMsgBuf[131072];
int svsHeaderValid;
bool g_archivingSnapshot;
int com_inServerFrame;
const dvar_s* sv_maxclients;
scr_const_t scr_const; // Included native GSC metadata tables refer to this.

namespace {
#if defined(KHZ_PRODUCTION_ADAPTER_TEST)
using namespace khz_archive;
#else
using namespace khz_archive_proposal;
#endif
unsigned comparisons, testGroups;
bool mainOwner = true, decoderNull, decoderWrongTime;
std::uint64_t tick;
dvar_s clientLimit;
playerState_s currentPlayer;
KHZ_ArchiveSidecarImage saved, altered;
serverStatic_t capturedNative; // Only its captured archive tuple is used.

struct NativeError : std::runtime_error {
    int code;
    NativeError(int kind, const char* message) : std::runtime_error(message), code(kind) {}
};
void check(bool value, const char* expression, int line) {
    ++comparisons;
    if (value) return;
    std::fprintf(stderr, "actual staged adapter contract failed at line%d: %s\n", line, expression);
    std::abort();
}
#define CHECK(value) check(bool(value), #value, __LINE__)
template<class F> void rejects(F&& callback, const char* part) {
    bool rejected = false;
    try { callback(); }
    catch (const NativeError& error) {
        rejected = std::string(error.what()).find(part) != std::string::npos;
    }
    CHECK(rejected);
}

void reset() {
    svsHeaderValid = 0; g_archivingSnapshot = false; com_inServerFrame = 0;
    mainOwner = true; decoderNull = decoderWrongTime = false;
    sv.state = SS_GAME; svs.initialized = 1; svs.time = 0;
    svs.nextArchivedSnapshotFrames = svs.nextArchivedSnapshotBuffer = 0;
    svs.nextCachedSnapshotEntities = svs.nextCachedSnapshotClients = svs.nextCachedSnapshotFrames = 0;
    std::memset(svs.cachedSnapshotFrames, 0, sizeof(svs.cachedSnapshotFrames));
    std::memset(svs.archivedSnapshotFrames, 0, sizeof(svs.archivedSnapshotFrames));
    std::memset(&svsHeader, 0, sizeof(svsHeader));
    std::memset(&g_archiveMsg, 0, sizeof(g_archiveMsg));
    std::memset(&currentPlayer, 0, sizeof(currentPlayer));
    currentPlayer.stats[STAT_HEALTH] = 100; currentPlayer.otherFlags = 4;
    clientLimit.current.integer = 16; sv_maxclients = &clientLimit;
    tick = 0; KHZ_ArchiveResetPolicy();
#if defined(KHZ_PRODUCTION_ADAPTER_TEST)
    const std::string diagnostics = KHZ_ArchiveDiagnosticsJSON();
    CHECK(diagnostics.find("\"sample_msec\":50") != std::string::npos);
    CHECK(diagnostics.find("\"committed_frames\":0") != std::string::npos);
    CHECK(diagnostics.find("\"committed_bytes\":0") != std::string::npos);
    CHECK(diagnostics.find("\"attempt_active\":false") != std::string::npos);
    CHECK(diagnostics.find("\"index_valid\":true") != std::string::npos);
#endif
}
void begin(int time) {
    svs.time = time; tick = std::uint64_t(time) + 1;
    svsHeader.nextCachedSnapshotEntities = svs.nextCachedSnapshotEntities;
    svsHeader.nextCachedSnapshotClients = svs.nextCachedSnapshotClients;
    svsHeaderValid = 1; g_archivingSnapshot = true;
    CHECK(KHZ_ArchiveSampleDue());
    std::memset(&g_archiveMsg, 0, sizeof(g_archiveMsg));
    g_archiveMsg.data = tempServerMsgBuf; g_archiveMsg.maxsize = MessageCapacity;
}
cachedSnapshot_t* fakeCache(int id, int time, bool delta, unsigned clientRows = 1) {
    auto* frame = &svs.cachedSnapshotFrames[svs.nextCachedSnapshotFrames % CacheFrameCapacity];
    *frame = {id, time, 0, svs.nextCachedSnapshotEntities,
        int(clientRows), svs.nextCachedSnapshotClients, int(delta)};
    for (unsigned row = 0; row != clientRows; ++row) {
        auto& data = svs.cachedSnapshotClients[svs.nextCachedSnapshotClients % CacheClientCapacity];
        std::memset(&data, 0, sizeof(data));
        data.cs.clientIndex = int(row); data.playerStateExists = 1;
        data.ps.otherFlags = 4; data.ps.stats[STAT_HEALTH] = 100;
        data.ps.origin[0] = float(time);
        ++svs.nextCachedSnapshotClients;
    }
    ++svs.nextCachedSnapshotFrames;
    svsHeader.nextCachedSnapshotClients = svs.nextCachedSnapshotClients;
    return frame;
}
void complete(int size, unsigned char pattern = 0x47) {
    std::memset(tempServerMsgBuf, pattern, size);
    g_archiveMsg.cursize = size;
    svsHeaderValid = 0; g_archivingSnapshot = false;
    KHZ_ArchiveCompleteMessage(&g_archiveMsg);
}
cachedSnapshot_t* full(int time, int size = 80, bool cache = true) {
    begin(time); KHZ_ArchiveEncodedFull();
    auto* frame = cache ? fakeCache(svs.nextArchivedSnapshotFrames, time, false) : nullptr;
    complete(size); return frame;
}
void copyActualCapturedTuple() {
    capturedNative.time = saved.nativeTime;
    capturedNative.nextArchivedSnapshotFrames = saved.nextFrame;
    capturedNative.nextArchivedSnapshotBuffer = saved.nextByte;
    std::memcpy(capturedNative.archivedSnapshotFrames, saved.descriptors.data(), sizeof(saved.descriptors));
}

void committedIdentityAndNativeLayout() {
    ++testGroups; reset();
    static_assert(sizeof(archivedSnapshot_s) == 8, "Real Win32 descriptor layout");
    static_assert(sizeof(cachedSnapshot_t) == 28, "Real Win32 cache descriptor layout");
    static_assert(sizeof(msg_t) == 40, "Real Win32 message layout");
    CHECK(!KHZ_ArchiveCommittedDescriptor(-1));
    CHECK(!KHZ_ArchiveCommittedDescriptor(0));
    auto* base = full(0);
    CHECK(KHZ_ArchiveCommittedDescriptor(0));
    CHECK(!KHZ_ArchiveCommittedDescriptor(1));
    CHECK(KHZ_ArchiveCachedFrameMatches(base));
    CHECK(KHZ_ArchiveDecodedTimeMatches(0, 0));
    CHECK(!KHZ_ArchiveDecodedTimeMatches(0, 1));
    CHECK(KHZ_ArchiveDecodedKindMatches(0, false));
    CHECK(!KHZ_ArchiveDecodedKindMatches(0, true));
    cachedSnapshot_t foreign = *base;
    CHECK(!KHZ_ArchiveCachedFrameMatches(&foreign));
    CHECK(!KHZ_ArchiveCachedFrameMatches(reinterpret_cast<const cachedSnapshot_t*>(
        reinterpret_cast<const unsigned char*>(base) + 1)));
    base->time = 1; CHECK(!KHZ_ArchiveCachedFrameMatches(base)); base->time = 0;
    base->usesDelta = 2; CHECK(!KHZ_ArchiveCachedFrameMatches(base)); base->usesDelta = 0;
    svs.archivedSnapshotFrames[0].size = 81; CHECK(!KHZ_ArchiveCommittedDescriptor(0));
    svs.archivedSnapshotFrames[0].size = 80;
    CHECK(KHZ_ArchiveCommittedDescriptor(0));
}
void overflowPhantomAndPostMessageGuards() {
    ++testGroups; reset(); full(0);
    begin(50); KHZ_ArchiveEncodedFull();
    auto* phantom = fakeCache(1, 50, false, 16);
    const int advancedCache = svs.nextCachedSnapshotFrames, advancedClients = svs.nextCachedSnapshotClients;
    g_archiveMsg.overflowed = 1; g_archiveMsg.cursize = MessageCapacity;
    svsHeaderValid = 0; g_archivingSnapshot = false;
    KHZ_ArchiveCompleteMessage(&g_archiveMsg);
    CHECK(svs.nextArchivedSnapshotFrames == 1 && svs.nextArchivedSnapshotBuffer == 80);
    CHECK(svs.nextCachedSnapshotFrames == advancedCache && svs.nextCachedSnapshotClients == advancedClients);
    CHECK(!KHZ_ArchiveCommittedDescriptor(1) && !KHZ_ArchiveCachedFrameMatches(phantom));
    auto* fresh = full(100);
    CHECK(KHZ_ArchiveCachedFrameMatches(fresh)); CHECK(!KHZ_ArchiveCachedFrameMatches(phantom));
    begin(150);
    CHECK(!KHZ_ArchiveCanUseDeltaBase(phantom)); CHECK(KHZ_ArchiveCanUseDeltaBase(fresh));
    KHZ_ArchiveEncodedDelta(fresh);
    auto* delta = fakeCache(2, 150, true);
    complete(40);
    CHECK(KHZ_ArchiveCachedFrameMatches(delta)); CHECK(KHZ_ArchiveDeltaReferenceMatches(2, 1));
    CHECK(!KHZ_ArchiveDeltaReferenceMatches(2, 2)); CHECK(!KHZ_ArchiveDeltaReferenceMatches(2, 0));
    CHECK(!KHZ_ArchiveDeltaReferenceMatches(1, 0));
    begin(200); KHZ_ArchiveEncodedFull();
    g_archiveMsg.cursize = 24; svsHeaderValid = 0; g_archivingSnapshot = false;
    msg_t foreign = g_archiveMsg;
    rejects([&] { KHZ_ArchiveCompleteMessage(&foreign); }, "bounded native message");
    CHECK(svs.nextArchivedSnapshotFrames == 3 && svs.nextArchivedSnapshotBuffer == 200);
    g_archiveMsg.data = tempServerMsgBuf + 1;
    rejects([&] { KHZ_ArchiveCompleteMessage(&g_archiveMsg); }, "bounded native message");
    g_archiveMsg.data = tempServerMsgBuf;
    g_archiveMsg.cursize = 0;
    rejects([&] { KHZ_ArchiveCompleteMessage(&g_archiveMsg); }, "metadata changed");
    CHECK(svs.nextArchivedSnapshotFrames == 3 && svs.nextArchivedSnapshotBuffer == 200);
    complete(24);
}
void cachedHeadroomAndFullAge() {
    ++testGroups; reset(); auto* base = full(0);
    const int originalClients = svs.nextCachedSnapshotClients;
    svs.nextCachedSnapshotFrames = 512; CHECK(!KHZ_ArchiveCachedFrameMatches(base));
    svs.nextCachedSnapshotFrames = 513;
    svs.nextCachedSnapshotClients = 4096; base->first_client = 63;
    CHECK(!KHZ_ArchiveCachedFrameMatches(base));
    base->first_client = 64; CHECK(KHZ_ArchiveCachedFrameMatches(base));
    base->first_client = 0; svs.nextCachedSnapshotClients = originalClients;
    begin(1000); CHECK(KHZ_ArchiveCanUseDeltaBase(base));
    KHZ_ArchiveEncodedFull(); complete(80);
    begin(1050); CHECK(!KHZ_ArchiveCanUseDeltaBase(base));
    KHZ_ArchiveEncodedFull(); complete(80);
}
void logicalDelayAndDecoderMismatch() {
    ++testGroups; reset();
    for (int time = 0; time <= 1000; time += 50) full(time);
    for (int now = 1000; now < 1050; ++now) {
        svs.time = now;
        int delay = 500;
        const auto* frame = KHZ_ArchiveLookup(&delay);
        CHECK(frame && frame->time == ((now - 500 + 49) / 50) * 50);
        CHECK(delay == 500);
    }
    svs.time = 1001; int delay = 2000;
    auto* oldest = KHZ_ArchiveLookup(&delay);
    CHECK(oldest && oldest->time == 0 && delay == 1001);
    decoderWrongTime = true; delay = 500;
    rejects([&] { KHZ_ArchiveLookup(&delay); }, "cache identity differs");
    decoderWrongTime = false;
    decoderNull = true; delay = 500;
    rejects([&] { KHZ_ArchiveLookup(&delay); }, "selection failed");
    decoderNull = false;
}
void byteSplitCopyAndEviction() {
    ++testGroups; reset(); full(0, 101, false);
    for (int id = 1; id != 256; ++id) full(id * 50, MessageCapacity, false);
    CHECK(svs.nextArchivedSnapshotBuffer == ByteCapacity - MessageCapacity + 101);
    begin(12800); KHZ_ArchiveEncodedFull();
    for (int i = 0; i < MessageCapacity; ++i) tempServerMsgBuf[i] = static_cast<unsigned char>(i * 17 + 3);
    g_archiveMsg.cursize = MessageCapacity; svsHeaderValid = 0; g_archivingSnapshot = false;
    const int start = svs.nextArchivedSnapshotBuffer;
    KHZ_ArchiveCompleteMessage(&g_archiveMsg);
    for (int i = 0; i < MessageCapacity; ++i)
        CHECK(svs.archivedSnapshotBuffer[(start + i) % ByteCapacity] ==
            static_cast<unsigned char>(i * 17 + 3));
    CHECK(svs.nextArchivedSnapshotBuffer == ByteCapacity + 101);
    CHECK(!KHZ_ArchiveCommittedDescriptor(0) && KHZ_ArchiveCommittedDescriptor(1));
    CHECK(svs.archivedSnapshotFrames[256].start == start && svs.archivedSnapshotFrames[256].size == MessageCapacity);
}
void sameProcessImageValidationAndLifecycle() {
    ++testGroups; reset(); full(0); full(50);
    CHECK(KHZ_ArchiveCopySidecar(&saved)); copyActualCapturedTuple();
    CHECK(KHZ_ArchiveValidateSidecar(saved, 50));
    CHECK(KHZ_ArchiveSidecarMatchesNativeImage(saved, capturedNative));
    capturedNative.time = 51; CHECK(!KHZ_ArchiveSidecarMatchesNativeImage(saved, capturedNative));
    capturedNative.time = 50;
    altered = saved; ++altered.nextByte; CHECK(!KHZ_ArchiveValidateSidecar(altered, 50));
    altered = saved; ++altered.policy.nextDueTime; CHECK(!KHZ_ArchiveValidateSidecar(altered, 50));
    altered = saved; altered.policy.entries[1].nativeTime = 0; CHECK(!KHZ_ArchiveValidateSidecar(altered, 50));
    altered = saved; altered.policy.availability[0] ^= UINT64_C(1) << 63;
    CHECK(!KHZ_ArchiveValidateSidecar(altered, 50));
    altered = saved; altered.descriptors[1].size = 81; CHECK(!KHZ_ArchiveValidateSidecar(altered, 50));
    full(100); CHECK(KHZ_ArchiveValidateSidecar(saved, 50));
    // Only synthetic archive metadata is reset here. No actual native bytes,
    // gameplay, VM, network fence or full-world restore is attested.
    svs.nextArchivedSnapshotFrames = saved.nextFrame;
    svs.nextArchivedSnapshotBuffer = saved.nextByte; svs.time = saved.nativeTime;
    std::memcpy(svs.archivedSnapshotFrames, saved.descriptors.data(), sizeof(saved.descriptors));
    KHZ_ArchiveRestoreSidecar(saved);
    CHECK(KHZ_ArchiveCopySidecar(&altered));
    CHECK(std::memcmp(&saved.policy, &altered.policy, sizeof(saved.policy)) == 0);
    mainOwner = false;
    CHECK(!KHZ_ArchiveCopySidecar(&altered)); CHECK(!KHZ_ArchiveValidateSidecar(saved, 50));
    CHECK(!KHZ_ArchiveSidecarMatchesNativeImage(saved, capturedNative));
    mainOwner = true;
    begin(100); KHZ_ArchiveEncodedFull(); // Simulate abandoned error-path encode.
    svsHeaderValid = 1; g_archivingSnapshot = true;
    sv.state = SS_DEAD; svs.initialized = 0; svs.nextArchivedSnapshotFrames = svs.nextArchivedSnapshotBuffer = 0;
    KHZ_ArchiveShutdownComplete();
    CHECK(!svsHeaderValid && !g_archivingSnapshot);
    sv.state = SS_GAME; svs.initialized = 1; KHZ_ArchiveResetPolicy();
    CHECK(!KHZ_ArchiveValidateSidecar(saved, 50)); CHECK(!KHZ_ArchiveCommittedDescriptor(0));
}
void actualInterpolationCode() {
    ++testGroups; reset();
    full(100); full(150); svs.time = 175; currentPlayer.origin[0] = 175.0f;
    float position[3] = {-8, -8, -8};
    CHECK(KHZ_ArchiveClientPosition(0, 125, position) && position[0] == 125.0f);
    CHECK(KHZ_ArchiveClientPosition(0, 163, position) && position[0] == 163.0f);
    CHECK(KHZ_ArchiveClientPosition(0, 175, position) && position[0] == 175.0f);
    CHECK(!KHZ_ArchiveClientPosition(16, 150, position));
    currentPlayer.stats[STAT_HEALTH] = 0; CHECK(!KHZ_ArchiveClientPosition(0, 150, position));
    currentPlayer.stats[STAT_HEALTH] = 100; currentPlayer.otherFlags |= 2;
    CHECK(!KHZ_ArchiveClientPosition(0, 150, position));
}
}

bool __cdecl Sys_IsMainThread() { return mainOwner; }
std::uint64_t KHZ_ServerTickNumber() { return tick; }
bool __cdecl SV_Loaded() { return sv.state == SS_GAME; }
int __cdecl SV_GetCurrentClientInfo(int client, playerState_s* player, clientState_s* state) {
    if (client != 0) return 0;
    *player = currentPlayer; std::memset(state, 0, sizeof(*state)); return 1;
}
cachedSnapshot_t* __cdecl SV_GetCachedSnapshotInternal(int id) {
    if (decoderNull) return nullptr;
    for (int index = svs.nextCachedSnapshotFrames - 1; index >= 0 && index >= svs.nextCachedSnapshotFrames - 512; --index) {
        auto* frame = &svs.cachedSnapshotFrames[index % 512];
        if (frame->archivedFrame != id) continue;
        if (decoderWrongTime) ++frame->time;
        return frame;
    }
    return nullptr;
}
void __cdecl Com_Error(errorParm_t code, const char* format, ...) {
    char message[1024]; va_list args; va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args); va_end(args);
    throw NativeError(int(code), message);
}
void MyAssertHandler(const char*, int, int, const char* format, ...) {
    char message[1024]; va_list args; va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args); va_end(args);
    throw NativeError(-1, message);
}
int main() {
    committedIdentityAndNativeLayout(); overflowPhantomAndPostMessageGuards();
    cachedHeadroomAndFullAge(); logicalDelayAndDecoderMismatch(); byteSplitCopyAndEviction();
    sameProcessImageValidationAndLifecycle(); actualInterpolationCode();
    std::printf("actual staged adapter synthetic contracts PASS: %u groups, %u comparisons; "
                "no native codec/map/world restore attestation\n", testGroups, comparisons);
}
