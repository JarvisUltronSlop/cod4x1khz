#include "khz_content.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace khz::content {
namespace {
struct Handle { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct Internet { HINTERNET value = nullptr; ~Internet() { if (value) WinHttpCloseHandle(value); } };
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    bool Open() {
        DWORD bytes = 0, size = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
            || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), 4, &bytes, 0) < 0
            || size > 65536) return false;
        object.resize(size);
        return BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0;
    }
    bool Add(const void* data, std::size_t length) {
        return length <= ULONG_MAX && BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const UCHAR*>(data)), ULONG(length), 0) >= 0;
    }
    bool Finish(Digest* result) { return result && BCryptFinishHash(hash, result->data(), ULONG(result->size()), 0) >= 0; }
};
struct Url { std::wstring host, path; INTERNET_PORT port = 0; bool secure = false; };
bool urlParts(std::string_view value, Url* result, std::string* origin) {
    if (value.empty() || value.size() > 1023 || value.find_first_of("\\#@\r\n\t ") != value.npos) return false;
    for (unsigned char c : value) if (c < 33 || c > 126) return false;
    std::wstring wide(value.begin(), value.end());
    URL_COMPONENTS components{}; components.dwStructSize = sizeof(components);
    components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength = DWORD(-1);
    components.dwUserNameLength = components.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), DWORD(wide.size()), 0, &components)
        || (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS)
        || !components.dwHostNameLength || components.dwUserNameLength || components.dwPasswordLength) return false;
    result->host.assign(components.lpszHostName, components.dwHostNameLength);
    result->path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (result->path.empty()) result->path = L"/";
    if (components.dwExtraInfoLength) result->path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    result->port = components.nPort; result->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    std::string host(result->host.begin(), result->host.end());
    for (char& c : host) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    *origin = std::string(result->secure ? "https://" : "http://") + host + ":" + std::to_string(result->port);
    return true;
}
bool privateHost(const std::wstring& host) {
    unsigned a, b, c, d; wchar_t trailing;
    if (host == L"localhost") return true;
    if (swscanf_s(host.c_str(), L"%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing, 1) != 4
        || a > 255 || b > 255 || c > 255 || d > 255) return false;
    return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}
bool plainPart(std::string_view text) {
    if (text.empty() || text.size() > 96 || text == "." || text == ".." || text.back() == '.' || text.back() == ' ') return false;
    for (unsigned char c : text) if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')
        && !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.') return false;
    const auto base = text.substr(0,text.find('.'));
    const auto equalsUpper = [](std::string_view value,std::string_view expected) {
        if (value.size()!=expected.size()) return false;
        for (unsigned i=0;i<value.size();++i) {
            const char c=value[i]>='a'&&value[i]<='z' ? char(value[i]-'a'+'A') : value[i];
            if (c!=expected[i]) return false;
        }
        return true;
    };
    if (equalsUpper(base,"CON") || equalsUpper(base,"PRN") || equalsUpper(base,"AUX") || equalsUpper(base,"NUL")
        || (base.size() == 4 && (equalsUpper(base.substr(0,3),"COM") || equalsUpper(base.substr(0,3),"LPT")) && base[3] >= '0' && base[3] <= '9')) return false;
    return true;
}
bool relativeParts(std::string_view path, std::vector<std::string_view>* pieces) {
    if (path.empty() || path.size() >= 240 || path.front() == '/' || path.back() == '/') return false;
    while (!path.empty()) {
        const auto split = path.find('/'); const auto part = path.substr(0, split);
        if (!plainPart(part)) return false;
        pieces->push_back(part);
        if (pieces->size() > 16) return false;
        if (split == path.npos) break;
        path.remove_prefix(split + 1);
    }
    return true;
}
bool noReparse(const std::filesystem::path& path, bool directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && (!directory || (attributes & FILE_ATTRIBUTE_DIRECTORY));
}
std::filesystem::path home, stage;
std::mutex lock;
std::condition_variable ready;
std::thread worker;
std::array<Request, QueueCapacity> requests{};
std::array<Completion, CompletionCapacity> completions{};
unsigned requestRead = 0, requestWrite = 0, completionRead = 0, completionWrite = 0;
std::atomic<bool> stopping{true}, running{false};
std::atomic<std::uint64_t> cancelledSession{0}, cancelledGeneration{0}, progressId{0}, progressBytes{0}, progressTotal{0};
std::atomic<bool> activeCancelled{false};
std::uint64_t activeSession = 0, activeGeneration = 0, highestRequestId = 0, bootNonce = 0;
std::uint64_t cancellationSerial = 0;
struct CancelledEpoch { std::uint64_t session = 0, generation = 0, serial = 0; };
std::array<CancelledEpoch, 64> cancellationHistory{};
unsigned cancellationWrite = 0;
bool cancelled(const Request& request, std::uint64_t serial) {
    if (stopping.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> guard(lock);
    (void)request; (void)serial;
    return activeCancelled.load(std::memory_order_acquire);
}
std::filesystem::path stageFile(const Request& request) { return stage / (std::to_string(bootNonce) + "-" + std::to_string(request.id) + "-" + std::to_string(request.session) + "-" + std::to_string(request.generation) + ".part"); }
void detail(Completion& result, Status status, const char* text) { result.status = status; std::snprintf(result.detail, sizeof(result.detail), "%s", text); }
bool fetch(const Request& request, std::uint64_t serial, Completion* result) {
    Url url; std::string origin;
    if (!urlParts(request.url, &url, &origin)) { detail(*result, Status::Invalid, "URL invalid"); return false; }
    Internet session{WinHttpOpen(L"cod4x1khz/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) { result->nativeError = GetLastError(); detail(*result, Status::Network, "HTTP session failed"); return false; }
    WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000);
    DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls));
    Internet connection{WinHttpConnect(session.value, url.host.c_str(), url.port, 0)};
    Internet http{connection.value ? WinHttpOpenRequest(connection.value, L"GET", url.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!http.value || !WinHttpSetOption(http.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))
        || !WinHttpSendRequest(http.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
        || !WinHttpReceiveResponse(http.value, nullptr)) {
        result->nativeError = GetLastError(); detail(*result, Status::Network, "HTTP transfer failed"); return false;
    }
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(http.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        result->nativeError = status; detail(*result, Status::Network, "HTTP status is not 200; redirects disabled"); return false;
    }
    const auto path = stageFile(request);
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Hash hash;
    if (file.value == INVALID_HANDLE_VALUE || !hash.Open()) { detail(*result, Status::Storage, "Cannot create verified staging file"); return false; }
    std::array<unsigned char, 65536> buffer{}; result->bytes = 0;
    for (;;) {
        if (cancelled(request, serial)) { detail(*result, Status::Cancelled, "Transfer cancelled by lifecycle generation"); return false; }
        DWORD bytes = 0, written = 0;
        if (!WinHttpReadData(http.value, buffer.data(), DWORD(buffer.size()), &bytes)) {
            result->nativeError = GetLastError(); detail(*result, Status::Network, "HTTP read failed"); return false;
        }
        if (!bytes) break;
        if (result->bytes + bytes > request.expectedBytes || result->bytes + bytes > MaxFileBytes) {
            detail(*result, Status::Size, "Transfer exceeds announced size"); return false;
        }
        if (!hash.Add(buffer.data(), bytes) || !WriteFile(file.value, buffer.data(), bytes, &written, nullptr) || written != bytes) {
            detail(*result, Status::Storage, "Staging write failed"); return false;
        }
        result->bytes += bytes; progressBytes.store(result->bytes, std::memory_order_relaxed);
    }
    Digest actual{};
    if (result->bytes != request.expectedBytes) { detail(*result, Status::Size, "Transfer truncated"); return false; }
    if (!hash.Finish(&actual) || actual != request.sha256) { detail(*result, Status::Hash, "SHA256 mismatch"); return false; }
    if (!FlushFileBuffers(file.value)) { detail(*result, Status::Storage, "Staging flush failed"); return false; }
    // Close before reopening for bounded ZIP directory verification.
    CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
    if (request.checkPure) {
        std::uint32_t checksum = 0, pure = 0;
        if (!VerifyIwd(path, request.checksumFeed, &checksum, &pure) || checksum != request.checksum || pure != request.pureChecksum) {
            detail(*result, Status::Pure, "Stock IWD/pure checksum mismatch"); return false;
        }
    }
    detail(*result, Status::Staged, "Verified content staged; filesystem barrier required");
    return true;
}
void work() {
    running.store(true, std::memory_order_release);
    while (!stopping.load(std::memory_order_acquire)) {
        Request request; std::uint64_t serial = 0;
        {
            std::unique_lock<std::mutex> guard(lock);
            ready.wait(guard, [] { return stopping.load() || requestRead != requestWrite; });
            if (stopping.load()) break;
            request = requests[requestRead++ & (QueueCapacity - 1)]; serial = cancellationSerial;
            activeSession = request.session; activeGeneration = request.generation;
            activeCancelled.store(!request.expectedBytes, std::memory_order_release);
        }
        Completion result; result.request = request; result.cancellationSerial = serial;
        progressId = request.id; progressBytes = 0; progressTotal = request.expectedBytes;
        for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            result.attempts = attempt;
            if (cancelled(request, serial)) { detail(result, Status::Cancelled, "Transfer cancelled"); break; }
            if (fetch(request, serial, &result)) break;
            if (result.status != Status::Network && result.status != Status::Size) break;
        }
        if (cancelled(request, serial)) detail(result, Status::Cancelled, "Transfer cancelled");
        if (result.status != Status::Staged) DeleteFileW(stageFile(request).c_str());
        {
            std::unique_lock<std::mutex> guard(lock);
            ready.wait(guard, [] { return stopping.load() || completionWrite - completionRead < CompletionCapacity; });
            if (stopping.load()) { DeleteFileW(stageFile(request).c_str()); break; }
            completions[completionWrite++ & (CompletionCapacity - 1)] = result;
        }
        progressId = 0;
    }
    running.store(false, std::memory_order_release);
}
unsigned read16(const unsigned char* p) { return unsigned(p[0]) | unsigned(p[1]) << 8; }
std::uint32_t read32(const unsigned char* p) { return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24; }
bool readAt(HANDLE file, std::uint64_t offset, void* data, DWORD length) {
    LARGE_INTEGER position; position.QuadPart = offset; DWORD bytes = 0;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, data, length, &bytes, nullptr) && bytes == length;
}
}

bool SafeAssetPath(std::string_view relative) {
    // Packet admission uses this path validator: no vector, string or suffix
    // allocation is permitted before a request enters the bounded worker.
    if (relative.empty() || relative.size()>=240 || relative.front()=='/' || relative.back()=='/') return false;
    std::array<std::string_view,3> parts{}; unsigned count=0;
    while (!relative.empty()) {
        const auto split=relative.find('/'); const auto part=relative.substr(0,split);
        if (count==parts.size() || !plainPart(part)) return false;
        parts[count++]=part;
        if (split==relative.npos) break;
        relative.remove_prefix(split+1);
    }
    if (count == 2 && parts[0] == "main") {
        const auto file=parts[1];
        const bool stockPrefix=file.size()>=3 && (file[0]=='i'||file[0]=='I') && (file[1]=='w'||file[1]=='W') && file[2]=='_';
        return file.ends_with(".iwd") && !stockPrefix;
    }
    if (count != 3) return false;
    if (parts[0] == "mods") return parts[2].ends_with(".iwd") || parts[2] == "mod.ff";
    if (parts[0] == "usermaps" && parts[2].starts_with(parts[1])) {
        const auto suffix=parts[2].substr(parts[1].size());
        return suffix==".ff" || suffix=="_load.ff" || suffix==".iwd";
    }
    return false;
}
bool ApprovedUrl(std::string_view value, std::string_view configuredOrigin) {
    if (configuredOrigin.empty()) return false;
    Url url, allowed; std::string origin, originAllowed;
    if (!urlParts(value, &url, &origin) || !urlParts(configuredOrigin, &allowed, &originAllowed)
        || allowed.path != L"/" || origin != originAllowed) return false;
    return url.secure || privateHost(url.host);
}
std::uint32_t Crc32(const void* data, std::size_t length, std::uint32_t initial) {
    auto crc = ~initial; const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
bool Sha256(const void* bytes, std::size_t length, Digest* result) { Hash hash; return result && hash.Open() && hash.Add(bytes, length) && hash.Finish(result); }
bool HashFile(const std::filesystem::path& path, Digest* result, std::uint64_t* size) {
    if (!result || !noReparse(path, false)) return false;
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Hash hash;
    if (file.value == INVALID_HANDLE_VALUE || !hash.Open()) return false;
    std::array<unsigned char, 65536> buffer{}; std::uint64_t total = 0;
    for (;;) {
        DWORD bytes = 0;
        if (!ReadFile(file.value, buffer.data(), DWORD(buffer.size()), &bytes, nullptr)) return false;
        if (!bytes) break;
        total += bytes;
        if (total > MaxFileBytes || !hash.Add(buffer.data(), bytes)) return false;
    }
    if (size) *size = total;
    return hash.Finish(result);
}
bool VerifyIwd(const std::filesystem::path& path, std::uint32_t feed, std::uint32_t* checksum, std::uint32_t* pure) {
    if (!checksum || !pure || !noReparse(path, false)) return false;
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size) || size.QuadPart < 22 || size.QuadPart > MaxFileBytes) return false;
    const DWORD tailSize = DWORD((std::min)(size.QuadPart, 65557ll));
    std::vector<unsigned char> tail(tailSize);
    if (!readAt(file.value, size.QuadPart - tailSize, tail.data(), tailSize)) return false;
    std::size_t eocd = tail.size();
    for (std::size_t i = tail.size() - 22;; --i) {
        if (read32(tail.data() + i) == 0x06054b50 && i + 22 + read16(tail.data() + i + 20) == tail.size()) { eocd = i; break; }
        if (!i) break;
    }
    if (eocd == tail.size()) return false;
    const auto* end = tail.data() + eocd;
    const unsigned entries = read16(end + 10);
    const std::uint32_t directorySize = read32(end + 12), directoryOffset = read32(end + 16);
    if (read16(end + 4) || read16(end + 6) || read16(end + 8) != entries || entries == 0xffff
        || directorySize > 16 * 1024 * 1024 || std::uint64_t(directoryOffset) + directorySize != std::uint64_t(size.QuadPart - tailSize + eocd)) return false;
    std::vector<unsigned char> directory(directorySize);
    if (!readAt(file.value, directoryOffset, directory.data(), directorySize)) return false;
    std::size_t at = 0; std::uint32_t crc = 0, pureCrc = feed;
    for (unsigned i = 0; i < entries; ++i) {
        if (at + 46 > directory.size() || read32(directory.data() + at) != 0x02014b50) return false;
        const auto* entry = directory.data() + at;
        const unsigned name = read16(entry + 28), extra = read16(entry + 30), comment = read16(entry + 32);
        const std::uint64_t next = std::uint64_t(at) + 46 + name + extra + comment;
        if (!name || name >= 256 || next > directory.size() || read16(entry + 34) || (read16(entry + 8) & 1)
            || (read16(entry + 10) != 0 && read16(entry + 10) != 8)) return false;
        std::string_view member(reinterpret_cast<const char*>(entry + 46), name);
        if (member.back() == '/') member.remove_suffix(1);
        std::vector<std::string_view> parts;
        if (!relativeParts(member, &parts)) return false;
        const std::uint32_t local = read32(entry + 42), compressed = read32(entry + 20), uncompressed = read32(entry + 24);
        std::array<unsigned char, 30> localHeader{};
        if (std::uint64_t(local) + 30 >= directoryOffset || !readAt(file.value, local, localHeader.data(), 30)
            || read32(localHeader.data()) != 0x04034b50 || read16(localHeader.data() + 26) != name
            || read16(localHeader.data() + 6) != read16(entry + 8) || read16(localHeader.data() + 8) != read16(entry + 10)
            || std::uint64_t(local) + 30 + name + read16(localHeader.data() + 28) + compressed > directoryOffset) return false;
        std::array<char, 256> localName{};
        if (!readAt(file.value, std::uint64_t(local) + 30, localName.data(), name)
            || std::memcmp(localName.data(), entry + 46, name)) return false;
        if (uncompressed) { crc = Crc32(entry + 16, 4, crc); pureCrc = Crc32(entry + 16, 4, pureCrc); }
        at = std::size_t(next);
    }
    if (at != directory.size()) return false;
    *checksum = crc; *pure = pureCrc;
    return true;
}
bool ConfinedPath(const std::filesystem::path& root, std::string_view relative, std::filesystem::path* result, bool createParents) {
    std::vector<std::string_view> pieces;
    if (!result || !relativeParts(relative, &pieces)) return false;
    std::error_code error;
    auto current = std::filesystem::absolute(root, error).lexically_normal();
    if (error || !current.is_absolute() || !noReparse(current, true)) return false;
    // Check every existing ancestor, including the caller-supplied root.
    auto ancestor = current;
    while (!ancestor.empty()) { if (!noReparse(ancestor, true)) return false; const auto parent = ancestor.parent_path(); if (parent == ancestor) break; ancestor = parent; }
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        current /= std::string(pieces[i]);
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT || (i + 1 < pieces.size() && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                || (i + 1 == pieces.size() && (attributes & FILE_ATTRIBUTE_DIRECTORY))) return false;
        } else if (i + 1 < pieces.size()) {
            if (!createParents || !CreateDirectoryW(current.c_str(), nullptr)) return false;
        }
    }
    *result = current;
    return true;
}
bool AtomicWrite(const std::filesystem::path& path, const void* bytes, std::size_t length) {
    if (length > 16 * 1024 * 1024 || (length && !bytes)) return false;
    static std::atomic<unsigned> serial{0};
    const auto temporary = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++serial);
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    DWORD written = 0;
    const bool ok = file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, bytes, DWORD(length), &written, nullptr)
        && written == length && FlushFileBuffers(file.value);
    if (file.value != INVALID_HANDLE_VALUE) { CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE; }
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temporary.c_str()); return false; }
    return true;
}
bool Start(const std::filesystem::path& assetHome, const std::filesystem::path& stagingRoot) {
    std::filesystem::path checked;
    if (worker.joinable() || !ConfinedPath(assetHome, ".khz-check", &checked) || !ConfinedPath(stagingRoot, ".khz-check", &checked)) return false;
    home = std::filesystem::absolute(assetHome).lexically_normal(); stage = std::filesystem::absolute(stagingRoot).lexically_normal();
    requestRead = requestWrite = completionRead = completionWrite = 0;
    cancellationSerial = 0; cancelledSession = cancelledGeneration = 0; progressId = progressBytes = progressTotal = 0;
    cancellationHistory.fill({}); cancellationWrite = 0;
    activeSession = activeGeneration = highestRequestId = 0; activeCancelled = false;
    LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
    bootNonce = std::uint64_t(counter.QuadPart) ^ (std::uint64_t(GetCurrentProcessId()) << 32);
    stopping.store(false, std::memory_order_release);
    worker = std::thread(work);
    return true;
}
bool Submit(const Request& request) {
    const bool emptyHash = std::all_of(request.sha256.begin(), request.sha256.end(), [](unsigned char value) { return value == 0; });
    if (!request.id || !request.session || !request.generation || !request.expectedBytes || request.expectedBytes > MaxFileBytes || emptyHash
        || !std::memchr(request.url, 0, sizeof(request.url)) || !std::memchr(request.origin, 0, sizeof(request.origin))
        || !std::memchr(request.relative, 0, sizeof(request.relative))
        || (request.stagingOnly ? request.relative[0] != 0 : !SafeAssetPath(request.relative))
        || !ApprovedUrl(request.url, request.origin) || (request.checkPure && !std::string_view(request.relative).ends_with(".iwd"))) return false;
    std::lock_guard<std::mutex> guard(lock);
    if (stopping.load() || requestWrite - requestRead >= QueueCapacity || request.id <= highestRequestId) return false;
    highestRequestId = request.id;
    requests[requestWrite++ & (QueueCapacity - 1)] = request;
    ready.notify_one(); return true;
}
bool SubmitVerifiedBlob(const Request& value) {
    Request request = value;
    if (request.relative[0] || request.checkPure) return false;
    request.stagingOnly = true;
    return Submit(request);
}
bool ReadDocument(std::string_view value, std::string_view configuredOrigin, std::vector<std::uint8_t>* output, unsigned maximum) {
    if (!output || maximum > 65536 || !maximum || !ApprovedUrl(value, configuredOrigin)) return false;
    Url url; std::string origin;
    if (!urlParts(value, &url, &origin)) return false;
    Internet session{WinHttpOpen(L"cod4x1khz/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000)) return false;
    DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls))) return false;
    Internet connection{WinHttpConnect(session.value, url.host.c_str(), url.port, 0)};
    Internet http{connection.value ? WinHttpOpenRequest(connection.value, L"GET", url.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!http.value || !WinHttpSetOption(http.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))
        || !WinHttpSendRequest(http.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
        || !WinHttpReceiveResponse(http.value, nullptr)) return false;
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(http.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) || status != 200) return false;
    std::vector<std::uint8_t> bytes; bytes.reserve(maximum);
    std::array<std::uint8_t, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(http.value, buffer.data(), DWORD(buffer.size()), &read)) return false;
        if (!read) break;
        if (bytes.size() + read > maximum) return false;
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
    }
    if (bytes.empty()) return false;
    *output = std::move(bytes);
    return true;
}
void Cancel(std::uint64_t session, std::uint64_t generation) {
    std::lock_guard<std::mutex> guard(lock);
    cancelledSession = session; cancelledGeneration = generation; ++cancellationSerial;
    bool found = false;
    for (auto& value : cancellationHistory)
        if (value.serial && value.session == session && value.generation == generation) {
            value.serial = cancellationSerial; found = true; break;
        }
    if (!found) cancellationHistory[cancellationWrite++ & 63] = {session, generation, cancellationSerial};
    if (!session || (activeSession == session && activeGeneration == generation)) activeCancelled.store(true, std::memory_order_release);
    // Queued old-generation requests are marked cancelled when reached; they
    // cannot silently become requests for a newly connected map.
    for (unsigned i = requestRead; i != requestWrite; ++i) {
        auto& request = requests[i & (QueueCapacity - 1)];
        if (!session || (request.session == session && request.generation == generation)) request.expectedBytes = 0;
    }
    ready.notify_all();
}
bool Poll(Completion* result) {
    if (!result) return false;
    std::lock_guard<std::mutex> guard(lock);
    if (completionRead == completionWrite) return false;
    *result = completions[completionRead++ & (CompletionCapacity - 1)]; ready.notify_all(); return true;
}
bool Discard(const Completion& result) {
    if (!result.request.id || !result.request.session || !result.request.generation) return false;
    const auto file = stageFile(result.request);
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return false;
    return DeleteFileW(file.c_str()) != FALSE;
}
Progress GetProgress() {
    Progress result; result.id = progressId.load(); result.bytes = progressBytes.load(); result.total = progressTotal.load(); result.running = running.load();
    std::lock_guard<std::mutex> guard(lock); result.queued = requestWrite - requestRead; return result;
}
bool Commit(const Completion& result, std::uint64_t session, std::uint64_t generation, std::filesystem::path* backup) {
    if (result.status != Status::Staged || result.request.stagingOnly || !session || result.request.session != session || result.request.generation != generation
        || !SafeAssetPath(result.request.relative)) return false;
    // Cancel still applies after network completion and before FS activation.
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& value : cancellationHistory)
            if (value.serial > result.cancellationSerial && (!value.session || (value.session == session && value.generation == generation))) return false;
    }
    const auto source = stageFile(result.request);
    Digest actual{}; std::uint64_t bytes = 0;
    if (!HashFile(source, &actual, &bytes) || actual != result.request.sha256 || bytes != result.request.expectedBytes) return false;
    if (result.request.checkPure) {
        std::uint32_t checksum = 0, pure = 0;
        if (!VerifyIwd(source, result.request.checksumFeed, &checksum, &pure) || checksum != result.request.checksum || pure != result.request.pureChecksum) return false;
    }
    std::filesystem::path destination;
    if (!ConfinedPath(home, result.request.relative, &destination, true)) return false;
    const auto old = destination.wstring() + L".khz-backup." + std::to_wstring(bootNonce) + L"." + std::to_wstring(result.request.id);
    const bool exists = GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists && !MoveFileExW(destination.c_str(), old.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (exists) MoveFileExW(old.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
        return false;
    }
    if (backup) *backup = exists ? std::filesystem::path(old) : std::filesystem::path{};
    return true;
}
bool StagedPath(const Completion& result, std::uint64_t session, std::uint64_t generation, std::filesystem::path* path) {
    if (!path || result.status != Status::Staged || !session || result.request.session != session || result.request.generation != generation) return false;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& value : cancellationHistory)
            if (value.serial > result.cancellationSerial && (!value.session || (value.session == session && value.generation == generation))) return false;
    }
    const auto source = stageFile(result.request);
    Digest digest{}; std::uint64_t size = 0;
    if (!HashFile(source, &digest, &size) || digest != result.request.sha256 || size != result.request.expectedBytes) return false;
    *path = source;
    return true;
}
void Stop() {
    stopping.store(true, std::memory_order_release); ready.notify_all();
    if (worker.joinable()) worker.join();
    progressId = 0;
}
}
