#include <universal/q_shared.h>
#include "khz_content_server.h"
#if defined(COD4X1KHZ) && defined(KISAK_MP)
#include "khz_content_protocol.h"
#include "khz_admin_server.h"
#include "khz_bots.h"
#include "khz_paths.h"
#include "khz_runtime.h"
#include "khz_asset_paths.h"
#include <content_server_policy.hpp>
#include <server_mp/server_mp.h>
#include <universal/com_files.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <database/database.h>
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace {
namespace content = khz::content;
namespace wire = khz::content::wire;
namespace policy = khz::content::server;
constexpr unsigned Capacity = policy::Slots;
constexpr std::uint64_t RequestTimeoutMs = 15000;
struct Job {
    policy::Pending pending{};
    wire::Metadata metadata{};
    wchar_t root[512]{}, physical[512]{};
    std::uint64_t epoch = 0, deadline = 0;
};
struct Result {
    policy::Pending pending{};
    wire::Metadata metadata{};
    wire::Error error = wire::Error::Storage;
    std::uint64_t epoch = 0, deadline = 0;
    bool success = false;
};
struct DeferredError { policy::Pending pending{}; wire::Error error = wire::Error::Invalid; std::uint64_t nextAllowed = 0; bool waiting = false; };
struct Counters {
    std::uint64_t accepted = 0, rejected = 0, queued = 0, hashed = 0, sent = 0, errors = 0, stale = 0;
} counters;
policy::Requests requests;
std::array<DeferredError, Capacity> errors{};
std::array<Job, Capacity> jobs{};
std::array<Result, Capacity> results{};
std::mutex workLock;
std::condition_variable workReady;
std::thread worker;
unsigned jobRead = 0, jobWrite = 0, resultRead = 0, resultWrite = 0;
std::atomic<bool> stopping{true};
std::atomic<std::uint64_t> epoch{1};
std::uint64_t observedSession = 0, observedMap = 0;
bool initialized = false;

void owner() { iassert(Sys_IsMainThread()); }
int slot(const client_t* client) {
    if (!client || !svs.clients) return -1;
    const auto begin = reinterpret_cast<std::uintptr_t>(svs.clients), address = reinterpret_cast<std::uintptr_t>(client);
    if (address < begin || (address - begin) % sizeof(client_t)) return -1;
    const auto index = (address - begin) / sizeof(client_t);
    return index < Capacity && sv_maxclients && index < unsigned(sv_maxclients->current.integer) ? int(index) : -1;
}
policy::Admission admission(const client_t* client) {
    policy::Admission value; const int index = slot(client);
    if (index < 0) return value;
    value.slot = unsigned(index); value.connected = client->header.state >= CS_CONNECTED;
    value.bot = KHZ_IsNativeBot(client); value.lifecycle = KHZ_AdminClientGeneration(client);
    value.session = KHZ_CurrentServerSession(); value.mapGeneration = std::uint32_t(sv_serverId_value);
    const auto* fingerprint = KHZ_AdminClientIdentity(client); if (fingerprint) value.fingerprint = *fingerprint;
    return value;
}
wire::Query query(const policy::Pending& pending) {
    wire::Query value; value.id = pending.id; value.session = pending.admission.session;
    value.generation = pending.admission.mapGeneration;
    std::memcpy(value.relative, pending.relative, sizeof(value.relative)); return value;
}
bool current(const policy::Pending& pending) {
    return pending.admission.slot < Capacity && svs.clients
        && policy::SameConnection(pending.admission, admission(svs.clients + pending.admission.slot));
}
void resetEpoch() {
    ++epoch; requests.Reset(); errors = {};
    std::lock_guard<std::mutex> guard(workLock);
    jobRead = jobWrite = resultRead = resultWrite = 0;
    workReady.notify_all();
}
bool cancelled(const Job& job) {
    return stopping.load(std::memory_order_acquire) || epoch.load(std::memory_order_acquire) != job.epoch
        || GetTickCount64() > job.deadline;
}
struct File { HANDLE value = INVALID_HANDLE_VALUE; ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE handle = nullptr;
    std::vector<unsigned char> object;
    ~Hash() { if (handle) BCryptDestroyHash(handle); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    bool Open() {
        DWORD size = 0, bytes = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
            || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &bytes, 0) < 0
            || !size || size > 65536) return false;
        object.resize(size); return BCryptCreateHash(algorithm, &handle, object.data(), size, nullptr, 0, 0) >= 0;
    }
};
bool lockedHash(const Job& job, wire::Metadata* metadata) {
    std::filesystem::path physical;
    // Revalidate every ancestor on the worker. Then verify the opened handle's
    // final path as well, so replacing a checked directory with a junction in
    // the gap before CreateFile cannot resolve outside the captured root.
    if (cancelled(job) || !content::ConfinedPath(job.root, job.pending.relative, &physical)
        || _wcsicmp(physical.c_str(), job.physical)) return false;
    File file{CreateFileW(job.physical, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    BY_HANDLE_FILE_INFORMATION information{}; wchar_t finalPath[1024]{};
    const DWORD length = file.value == INVALID_HANDLE_VALUE ? 0 : GetFinalPathNameByHandleW(file.value, finalPath, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= 1024 || std::wcsncmp(finalPath, L"\\\\?\\", 4)
        || _wcsicmp(finalPath + 4, job.physical) || !GetFileInformationByHandle(file.value, &information)
        || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    const std::uint64_t bytes = (std::uint64_t(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    if (!bytes || bytes > content::MaxFileBytes) return false;
    Hash hash; if (!hash.Open()) return false;
    std::array<unsigned char, 65536> buffer{}; std::uint64_t read = 0;
    while (read < bytes) {
        if (cancelled(job)) return false;
        DWORD count = 0;
        if (!ReadFile(file.value, buffer.data(), DWORD(buffer.size()), &count, nullptr) || !count
            || read + count > bytes || BCryptHashData(hash.handle, buffer.data(), count, 0) < 0) return false;
        read += count;
    }
    if (BCryptFinishHash(hash.handle, metadata->sha256.data(), DWORD(metadata->sha256.size()), 0) < 0) return false;
    metadata->bytes = bytes;
    if (metadata->checkPure) {
        std::uint32_t checksum = 0, pure = 0;
        // This outer read-only-sharing handle remains open across both passes:
        // the announced SHA/size and native checksum refer to the same file.
        if (cancelled(job) || !content::VerifyIwd(physical, metadata->checksumFeed, &checksum, &pure)
            || checksum != metadata->checksum || pure != metadata->pureChecksum) return false;
    }
    return !cancelled(job);
}
void work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> guard(workLock);
            workReady.wait(guard, [] { return stopping.load(std::memory_order_acquire) || jobRead != jobWrite; });
            if (stopping.load(std::memory_order_acquire)) return;
            job = jobs[jobRead++ % Capacity];
        }
        Result result; result.pending = job.pending; result.metadata = job.metadata; result.epoch = job.epoch; result.deadline = job.deadline;
        try { result.success = lockedHash(job, &result.metadata); } catch (...) { result.success = false; }
        if (cancelled(job)) result.error = wire::Error::Stale;
        {
            std::unique_lock<std::mutex> guard(workLock);
            workReady.wait(guard, [&result] { return stopping.load(std::memory_order_acquire)
                || result.epoch != epoch.load(std::memory_order_acquire) || resultWrite - resultRead < Capacity; });
            if (stopping.load(std::memory_order_acquire)) return;
            // Backpressure retains this single bounded result until the owner
            // drains space. Epoch reset discards work from the previous map.
            if (result.epoch == epoch.load(std::memory_order_acquire) && resultWrite - resultRead < Capacity)
                results[resultWrite++ % Capacity] = result;
        }
    }
}
bool start() {
    if (worker.joinable()) return true;
    stopping.store(false, std::memory_order_release);
    try { worker = std::thread(work); } catch (...) { stopping.store(true, std::memory_order_release); return false; }
    return true;
}
void defer(const policy::Pending& pending, wire::Error error) {
    if (pending.admission.slot >= Capacity) return;
    ++counters.rejected; auto& rejected = errors[pending.admission.slot]; const auto now = GetTickCount64();
    if (policy::SameConnection(rejected.pending.admission, pending.admission) && (rejected.waiting || now < rejected.nextAllowed)) return;
    rejected = DeferredError{pending, error, now + 100, true};
}
void sendError(const policy::Pending& pending, wire::Error error) {
    if (!current(pending)) { ++counters.stale; return; }
    std::string text;
    if (wire::FormatRejection(wire::Rejection{query(pending), error}, &text)) {
        SV_SendServerCommand(svs.clients + pending.admission.slot, SV_CMD_RELIABLE, "%s", text.c_str()); ++counters.errors;
    }
}
bool capturedPath(const char* root, const char* relative, const std::filesystem::path* expected, Job* job) {
    if (!root || !*root) return false;
    std::filesystem::path physical;
    if (!content::ConfinedPath(root, relative, &physical)) return false;
    if (expected) {
        std::error_code error;
        const auto native = std::filesystem::absolute(*expected, error).lexically_normal();
        if (error || _wcsicmp(native.c_str(), physical.c_str())) return false;
    }
    std::error_code error;
    const auto absoluteRoot = std::filesystem::absolute(root, error).lexically_normal(); if (error) return false;
    const auto rootPath = absoluteRoot.wstring(), path = physical.wstring();
    if (rootPath.size() >= std::size(job->root) || path.size() >= std::size(job->physical)) return false;
    std::wcscpy(job->root, rootPath.c_str()); std::wcscpy(job->physical, path.c_str()); return true;
}
bool resolve(const policy::Pending& pending, Job* job, wire::Error* error) {
    *error = wire::Error::Denied;
    if (!sv_allowDownload || !sv_allowDownload->current.enabled || !sv_wwwBaseURL || !sv_wwwBaseURL->current.string[0]) { *error = wire::Error::Disabled; return false; }
    const auto relative = std::string_view(pending.relative);
    const char* mod = fs_gameDirVar ? fs_gameDirVar->current.string : "";
    const char* map = sv_mapname ? sv_mapname->current.string : "";
    if (!content::SafeAssetPath(relative) || !policy::AllowedScope(relative, mod, map)) return false;
    job->pending = pending; job->metadata.query = query(pending); job->metadata.checksumFeed = fs_checksumFeed;
    std::string origin(sv_wwwBaseURL->current.string);
    while (!origin.empty() && origin.back() == '/') origin.pop_back();
    const std::string url = origin + "/" + std::string(relative);
    if (url.size() >= sizeof(job->metadata.url) || !content::ApprovedUrl(url, origin)) return false;
    std::memcpy(job->metadata.url, url.c_str(), url.size() + 1);
    if (relative.ends_with(".iwd")) {
        for (auto* search = fs_searchpaths; search; search = search->next) {
            const auto* iwd = search->iwd;
            if (!iwd || search->ignore || search->bLocalized || (!iwd->referenced && !I_strnicmp(iwd->iwdGamename, "main", 4))) continue;
            const std::string listed = std::string(iwd->iwdGamename) + "/" + iwd->iwdBasename + ".iwd";
            if (relative != listed) continue;
            job->metadata.checkPure = true; job->metadata.checksum = std::uint32_t(iwd->checksum); job->metadata.pureChecksum = std::uint32_t(iwd->pure_checksum);
            // IWD search-path order decides which physical file is authorized;
            // accept only project runtime/home roots and the exact listed path.
            const std::filesystem::path expected(iwd->iwdFilename);
            if (capturedPath(KHZ_HomeRoot(), pending.relative, &expected, job)
                || capturedPath(KHZ_RuntimeRoot(), pending.relative, &expected, job)) return true;
            *error = wire::Error::Storage; return false;
        }
    } else {
        // DB name list describes the loaded fastfiles, not arbitrary files on
        // disk. Stock zone names cannot authorize a usermaps/mods look-alike.
        std::string names(DB_ReferencedFFNameList()); std::string_view rest(names);
        while (!rest.empty()) {
            const auto split = rest.find(' '); const auto listed = rest.substr(0, split);
            if (relative.size() == listed.size() + 3 && relative.substr(0, listed.size()) == listed && relative.substr(listed.size()) == ".ff") {
                job->metadata.checkPure = false; job->metadata.checksum = job->metadata.pureChecksum = 0;
                khz::assets::Fastfile selected;
                if (khz::assets::ReferencedFastfile(KHZ_HomeRoot(), KHZ_RuntimeRoot(), mod, map, listed, &selected)
                    && selected.relative == relative &&
                    (capturedPath(KHZ_HomeRoot(), pending.relative, &selected.physical, job)
                        || capturedPath(KHZ_RuntimeRoot(), pending.relative, &selected.physical, job))) return true;
                *error = wire::Error::Storage; return false;
            }
            if (split == rest.npos) break; rest.remove_prefix(split + 1);
        }
    }
    *error = wire::Error::Missing; return false;
}
}

void KHZ_ContentServerInit() { owner(); if (initialized) return; initialized = true; observedSession = observedMap = 0; counters = {}; requests.Reset(); }
bool KHZ_ContentServerClientCommand(client_t* client, const char* text) {
    owner();
    if (!text || std::strncmp(text, "khz_content", 11) || (text[11] && text[11] != ' ')) return false;
    if (!initialized) KHZ_ContentServerInit();
    const auto accepted = admission(client); if (!policy::Accepted(accepted)) { ++counters.rejected; return true; }
    const std::uint64_t session = KHZ_CurrentServerSession(), map = std::uint32_t(sv_serverId_value);
    if (session != observedSession || map != observedMap) { resetEpoch(); observedSession = session; observedMap = map; }
    const std::size_t length = strnlen(text, wire::MaximumText + 1);
    wire::Query parsed;
    if (length > wire::MaximumText || !wire::ParseQuery(std::string_view(text, length), &parsed)) { ++counters.rejected; return true; }
    policy::Pending pending; pending.admission = accepted; pending.id = parsed.id;
    std::memcpy(pending.relative, parsed.relative, sizeof(pending.relative));
    if (parsed.session != accepted.session || parsed.generation != accepted.mapGeneration) { ++counters.stale; return true; }
    const auto status = requests.Put(pending, GetTickCount64());
    if (status == policy::Enqueue::Accepted) ++counters.accepted;
    else if (status != policy::Enqueue::Duplicate) defer(pending, status == policy::Enqueue::Invalid ? wire::Error::Invalid : wire::Error::Busy);
    return true;
}
void KHZ_ContentServerService() {
    owner(); if (!initialized || com_inServerFrame) return;
    const std::uint64_t session = KHZ_CurrentServerSession(), map = std::uint32_t(sv_serverId_value);
    if (session != observedSession || map != observedMap) { resetEpoch(); observedSession = session; observedMap = map; }
    for (auto& rejected : errors) if (rejected.waiting) { sendError(rejected.pending, rejected.error); rejected.waiting = false; }
    for (unsigned i = 0; i < Capacity; ++i) {
        Result result;
        { std::lock_guard<std::mutex> guard(workLock); if (resultRead == resultWrite) break; result = results[resultRead++ % Capacity]; }
        workReady.notify_one();
        requests.Finish(result.pending);
        if (result.epoch != epoch.load(std::memory_order_acquire) || !current(result.pending)) { ++counters.stale; continue; }
        if (GetTickCount64() > result.deadline) { sendError(result.pending, wire::Error::Stale); continue; }
        if (!result.success) { sendError(result.pending, result.error); continue; }
        ++counters.hashed; std::string text;
        if (!wire::FormatMetadata(result.metadata, &text)) { sendError(result.pending, wire::Error::Denied); continue; }
        SV_SendServerCommand(svs.clients + result.pending.admission.slot, SV_CMD_RELIABLE, "%s", text.c_str()); ++counters.sent;
    }
    // Limit native FS search and descriptor creation to two requests per pass.
    for (unsigned i = 0; i < 2; ++i) {
        policy::Pending pending; if (!requests.Take(&pending)) break;
        if (!current(pending)) { requests.Finish(pending); ++counters.stale; continue; }
        Job job; wire::Error error = wire::Error::Storage; bool resolved = false;
        if (FS_Initialized()) { try { resolved = resolve(pending, &job, &error); } catch (...) { error = wire::Error::Storage; } }
        if (!resolved || !start()) { requests.Finish(pending); sendError(pending, resolved ? wire::Error::Storage : error); continue; }
        job.epoch = epoch.load(std::memory_order_acquire); job.deadline = GetTickCount64() + RequestTimeoutMs;
        bool queued = false;
        { std::lock_guard<std::mutex> guard(workLock); if (jobWrite - jobRead < Capacity) { jobs[jobWrite++ % Capacity] = job; queued = true; } }
        if (queued) { ++counters.queued; workReady.notify_one(); }
        else { requests.Finish(pending); sendError(pending, wire::Error::Busy); }
    }
}
void KHZ_ContentServerErrorCleanup() { owner(); if (initialized) { resetEpoch(); observedSession = observedMap = 0; } }
void KHZ_ContentServerShutdown() {
    owner(); stopping.store(true, std::memory_order_release); ++epoch; workReady.notify_all();
    if (worker.joinable()) worker.join();
    { std::lock_guard<std::mutex> guard(workLock); jobRead = jobWrite = resultRead = resultWrite = 0; }
    requests.Reset(); errors = {}; initialized = false; observedSession = observedMap = 0;
}
const char* KHZ_ContentServerStatsJSON() {
    owner(); static char text[512];
    std::snprintf(text, sizeof(text), "{\"accepted\":%llu,\"rejected\":%llu,\"queued\":%llu,\"hashed\":%llu,\"sent\":%llu,\"errors\":%llu,\"stale\":%llu}",
        static_cast<unsigned long long>(counters.accepted), static_cast<unsigned long long>(counters.rejected), static_cast<unsigned long long>(counters.queued),
        static_cast<unsigned long long>(counters.hashed), static_cast<unsigned long long>(counters.sent), static_cast<unsigned long long>(counters.errors), static_cast<unsigned long long>(counters.stale));
    return text;
}
#endif
