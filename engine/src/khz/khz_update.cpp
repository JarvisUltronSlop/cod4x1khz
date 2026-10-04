#include "khz_update.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace khz::update {
namespace {
constexpr char Identity[] = "KHZ_RELEASE_PROJECT=cod4x1khz;protocol=10002;format=1";
char hexDigit(unsigned n) { return "0123456789abcdef"[n & 15]; }
std::string hex(const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data); std::string result(length * 2, '0');
    for (std::size_t i = 0; i < length; ++i) { result[2*i] = hexDigit(bytes[i] >> 4); result[2*i+1] = hexDigit(bytes[i]); }
    return result;
}
bool unhex(std::string_view text, void* data, std::size_t length) {
    if (text.size() != length * 2) return false;
    auto* bytes = static_cast<std::uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        unsigned value = 0;
        for (unsigned half = 0; half < 2; ++half) {
            const char c = text[2*i+half];
            if (c >= '0' && c <= '9') value = value * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') value = value * 16 + c - 'a' + 10;
            else return false; // One canonical case; no ambiguous signed text.
        }
        bytes[i] = std::uint8_t(value);
    }
    return true;
}
bool number(std::string_view text, std::uint64_t* output) {
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0')) return false;
    std::uint64_t value = 0; const auto parsed = std::from_chars(text.data(), text.data()+text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size()) return false;
    *output = value; return true;
}
bool configuredKey(const PublicKey& key) {
    BCRYPT_RSAKEY_BLOB header{}; std::memcpy(&header, key.data(), sizeof(header));
    return header.Magic == BCRYPT_RSAPUBLIC_MAGIC && header.BitLength == 2048 && header.cbPublicExp == 3
        && header.cbModulus == 256 && !header.cbPrime1 && !header.cbPrime2
        && key[24] == 1 && key[25] == 0 && key[26] == 1;
}
bool read(const std::filesystem::path& path, std::string* value, std::uint64_t maximum) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    std::error_code error; const auto size = std::filesystem::file_size(path, error);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || error || size > maximum) return false;
    std::ifstream input(path, std::ios::binary); if (!input) return false;
    value->resize(std::size_t(size)); return bool(input.read(value->data(), std::streamsize(size)));
}
bool write(const std::filesystem::path& path, std::string_view value) { return content::AtomicWrite(path, value.data(), value.size()); }
Result failure(const char* text, bool recoverable = false) { Result result; result.recoverable = recoverable; std::snprintf(result.detail, sizeof(result.detail), "%s", text); return result; }
bool path(const std::filesystem::path& root, std::string_view relative, std::filesystem::path* result, bool create = false) { return content::ConfinedPath(root, relative, result, create); }
std::string document(const Manifest& manifest) { std::string text; if (!CanonicalBody(manifest, &text)) return {}; text += "signature=" + hex(manifest.signature.data(), manifest.signature.size()) + "\n"; return text; }
struct Journal {
    std::string state;
    std::uint64_t previous = 0;
    std::array<content::Digest, InstalledPaths.size()> oldHashes{};
    std::array<bool, InstalledPaths.size()> oldPresent{};
    Manifest manifest;
};
std::string journalText(const Journal& journal) {
    std::string text = "KHZ_UPDATE_JOURNAL_2\nstate=" + journal.state + "\nprevious=" + std::to_string(journal.previous) + "\n";
    for (unsigned i = 0; i < InstalledPaths.size(); ++i)
        text += "old=" + std::string(InstalledPaths[i]) + "\t" + (journal.oldPresent[i] ? hex(journal.oldHashes[i].data(), 32) : "absent") + "\n";
    text += document(journal.manifest); return text;
}
bool readJournal(const std::filesystem::path& root, const Policy& policy, Journal* result) {
    std::filesystem::path file; std::string text;
    if (!path(root, KHZ_UPDATE_DIRECTORY "/transaction.txt", &file) || !read(file, &text, MaxManifestBytes + 1024)) return false;
    constexpr std::string_view prefix = "KHZ_UPDATE_JOURNAL_2\nstate=";
    if (!text.starts_with(prefix)) return false;
    std::string_view view(text); view.remove_prefix(prefix.size());
    auto end = view.find('\n'); if (end == view.npos) return false;
    Journal journal; journal.state = std::string(view.substr(0,end)); view.remove_prefix(end+1);
    if (journal.state != "prepared" && journal.state != "committing" && journal.state != "committed" && journal.state != "rolledback") return false;
    if (!view.starts_with("previous=")) return false; view.remove_prefix(9);
    end = view.find('\n'); if (end == view.npos || !number(view.substr(0,end), &journal.previous)) return false; view.remove_prefix(end+1);
    for (unsigned i = 0; i < InstalledPaths.size(); ++i) {
        const std::string expected = "old=" + std::string(InstalledPaths[i]) + "\t";
        if (!view.starts_with(expected)) return false; view.remove_prefix(expected.size());
        end = view.find('\n'); if (end == view.npos) return false;
        const auto hash = view.substr(0,end);
        journal.oldPresent[i] = hash != "absent";
        if ((!journal.oldPresent[i] && i < PackagePaths.size())
            || (journal.oldPresent[i] && !unhex(hash, journal.oldHashes[i].data(), 32))) return false;
        view.remove_prefix(end+1);
    }
    if (!VerifyManifest(view, policy, journal.previous, &journal.manifest)) return false;
    *result = std::move(journal); return true;
}
bool saveJournal(const std::filesystem::path& root, const Journal& journal) { std::filesystem::path file; return path(root, KHZ_UPDATE_DIRECTORY "/transaction.txt", &file, true) && write(file, journalText(journal)); }
bool matches(const std::filesystem::path& file, const content::Digest& hash, std::uint64_t expected = 0) {
    content::Digest actual{}; std::uint64_t size = 0; return content::HashFile(file, &actual, &size) && actual == hash && (!expected || size == expected);
}
bool replaceCopy(const std::filesystem::path& source, const std::filesystem::path& destination, const content::Digest& expected) {
    const auto temporary = destination.wstring() + L".khz-update-copy";
    const DWORD attributes = GetFileAttributesW(temporary.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    bool ready = attributes != INVALID_FILE_ATTRIBUTES && matches(temporary, expected);
    if (!ready) {
        if (attributes != INVALID_FILE_ATTRIBUTES && !DeleteFileW(temporary.c_str())) return false;
        if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE) || !matches(temporary, expected)) { DeleteFileW(temporary.c_str()); return false; }
    }
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool durable = file != INVALID_HANDLE_VALUE && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!durable || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temporary.c_str()); return false; }
    return matches(destination, expected);
}
bool installedVersion(const std::filesystem::path& root, std::uint64_t fallback, std::uint64_t* value) {
    std::filesystem::path file; if (!path(root, KHZ_UPDATE_DIRECTORY "/version.txt", &file, true)) return false;
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) { *value = fallback; return true; }
    std::string text; if (!read(file, &text, 64) || text.empty() || text.back() != '\n') return false;
    text.pop_back(); return number(text, value);
}
bool saveVersion(const std::filesystem::path& root, std::uint64_t version) { std::filesystem::path file; return path(root, KHZ_UPDATE_DIRECTORY "/version.txt", &file, true) && write(file, std::to_string(version)+"\n"); }
Result rollback(const std::filesystem::path& root, Journal& journal) {
    // Validate every old backup and every new optional mirror before touching
    // any destination. A missing pre-update mirror is removed only if it still
    // contains the exact verified client bytes installed by this transaction.
    std::array<std::filesystem::path, InstalledPaths.size()> backups{}, targets{};
    for (unsigned i = 0; i < InstalledPaths.size(); ++i) {
        if (!path(root, InstalledPaths[i], &targets[i])
            || (journal.oldPresent[i] && (!path(root, KHZ_UPDATE_DIRECTORY "/rollback/" + std::to_string(journal.manifest.version) + "/" + InstalledPaths[i], &backups[i]) || !matches(backups[i], journal.oldHashes[i]))))
            return failure("Rollback backup missing or altered", true);
        if (!journal.oldPresent[i] && GetFileAttributesW(targets[i].c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto& source = journal.manifest.files[InstalledSourceIndexes[i]];
            if (!matches(targets[i], source.sha256, source.bytes)) return failure("New optional mirror altered; refuse rollback removal", true);
        }
    }
    for (unsigned i = 0; i < InstalledPaths.size(); ++i) {
        if (journal.oldPresent[i]) {
            if (!replaceCopy(backups[i], targets[i], journal.oldHashes[i])) return failure("Rollback replacement failed; retry recovery before launch", true);
        } else if (GetFileAttributesW(targets[i].c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(targets[i].c_str()))
            return failure("Optional mirror rollback removal failed; retry recovery before launch", true);
    }
    if (!saveVersion(root, journal.previous)) return failure("Rollback version commit failed", true);
    journal.state = "rolledback";
    if (!saveJournal(root, journal)) return failure("Rollback journal commit failed", true);
    Result result; result.rolledBack = true; std::snprintf(result.detail, sizeof(result.detail), "Previous root/runtime binaries and mirror state restored"); return result;
}
}

bool Configured(const Policy& policy) {
    if (!configuredKey(policy.trustedKey) || !std::memchr(policy.manifestUrl, 0, sizeof(policy.manifestUrl))
        || !std::memchr(policy.origin, 0, sizeof(policy.origin)) || !content::ApprovedUrl(policy.manifestUrl, policy.origin)) return false;
    return policy.privateHttpFixture || std::string_view(policy.manifestUrl).starts_with("https://");
}
bool CanonicalBody(const Manifest& manifest, std::string* output) {
    if (!output || !manifest.version) return false;
    std::string text = "KHZ_UPDATE_1\nproject=cod4x1khz\nplatform=" + std::string(Platform) + "\nprotocol=10002\nversion=" + std::to_string(manifest.version) + "\n" + std::string(ClientMirrors) + "files=2\n";
    for (unsigned i = 0; i < 2; ++i) {
        const auto& file = manifest.files[i];
        if (!std::memchr(file.name, 0, sizeof(file.name)) || std::strcmp(file.name, PackagePaths[i]) || !std::memchr(file.url, 0, sizeof(file.url))
            || !file.bytes || file.bytes > content::MaxFileBytes || std::all_of(file.sha256.begin(), file.sha256.end(), [](unsigned char c) { return !c; })) return false;
        const std::string_view url(file.url); if (url.empty() || url.find_first_of("\t\r\n ") != url.npos) return false;
        text += "file=" + std::string(file.name) + "\t" + std::to_string(file.bytes) + "\t" + hex(file.sha256.data(), file.sha256.size()) + "\t" + std::string(url) + "\n";
    }
    if (text.size() + 523 > MaxManifestBytes) return false;
    *output = std::move(text); return true;
}
bool VerifySignature(std::string_view body, const Signature& signature, const PublicKey& publicKey) {
    if (body.empty() || body.size() > MaxManifestBytes || !configuredKey(publicKey)) return false;
    content::Digest digest{}; if (!content::Sha256(body.data(), body.size(), &digest)) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    bool valid = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0
        && BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, const_cast<PUCHAR>(publicKey.data()), ULONG(publicKey.size()), 0) >= 0;
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    if (valid) valid = BCryptVerifySignature(key, &padding, digest.data(), ULONG(digest.size()), const_cast<PUCHAR>(signature.data()), ULONG(signature.size()), BCRYPT_PAD_PKCS1) >= 0;
    if (key) BCryptDestroyKey(key); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return valid;
}
bool VerifyManifest(std::string_view text, const Policy& policy, std::uint64_t installed, Manifest* output) {
    if (!output || !Configured(policy) || text.size() > MaxManifestBytes || text.find('\0') != text.npos || text.find('\r') != text.npos) return false;
    const std::string prefix = "KHZ_UPDATE_1\nproject=cod4x1khz\nplatform=" + std::string(Platform) + "\nprotocol=10002\nversion=";
    if (!text.starts_with(prefix)) return false;
    const auto original = text; text.remove_prefix(prefix.size());
    auto end = text.find('\n'); Manifest manifest;
    if (end == text.npos || !number(text.substr(0,end), &manifest.version) || manifest.version <= installed) return false;
    text.remove_prefix(end+1); if (!text.starts_with(ClientMirrors)) return false; text.remove_prefix(ClientMirrors.size());
    if (!text.starts_with("files=2\n")) return false; text.remove_prefix(8);
    for (unsigned i = 0; i < 2; ++i) {
        const std::string key = "file=" + std::string(PackagePaths[i]) + "\t";
        if (!text.starts_with(key)) return false; text.remove_prefix(key.size());
        auto& file = manifest.files[i]; std::strcpy(file.name, PackagePaths[i]);
        end = text.find('\t'); if (end == text.npos || !number(text.substr(0,end), &file.bytes)) return false; text.remove_prefix(end+1);
        end = text.find('\t'); if (end == text.npos || !unhex(text.substr(0,end), file.sha256.data(), file.sha256.size())) return false; text.remove_prefix(end+1);
        end = text.find('\n'); if (end == text.npos || end >= sizeof(file.url)) return false;
        std::memcpy(file.url, text.data(), end); text.remove_prefix(end+1);
        if (!content::ApprovedUrl(file.url, policy.origin)) return false;
    }
    if (!text.starts_with("signature=")) return false; text.remove_prefix(10);
    if (text.size() != 513 || text.back() != '\n' || !unhex(text.substr(0,512), manifest.signature.data(), manifest.signature.size())) return false;
    std::string body;
    if (!CanonicalBody(manifest, &body) || original.size() != body.size() + 523 || !original.starts_with(body)
        || !VerifySignature(body, manifest.signature, policy.trustedKey)) return false;
    *output = manifest; return true;
}
bool FetchManifest(const Policy& policy, std::uint64_t installed, Manifest* result) {
    std::vector<std::uint8_t> bytes;
    return Configured(policy) && content::ReadDocument(policy.manifestUrl, policy.origin, &bytes, MaxManifestBytes)
        && VerifyManifest(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), policy, installed, result);
}
const char* BinaryIdentity() { return Identity; }
bool VerifyBinary(const std::filesystem::path& file) {
    std::string bytes;
    if (!read(file, &bytes, 128 * 1024 * 1024) || bytes.size() < 256 || bytes[0] != 'M' || bytes[1] != 'Z'
        || bytes.find(Identity) == bytes.npos) return false;
    std::uint32_t offset = 0; std::memcpy(&offset, bytes.data()+60, 4);
    if (offset < 64 || std::uint64_t(offset) + 24 > bytes.size() || std::memcmp(bytes.data()+offset, "PE\0\0", 4)) return false;
    auto u16 = [&](std::size_t at) { std::uint16_t value; std::memcpy(&value, bytes.data()+at, 2); return value; };
    auto u32 = [&](std::size_t at) { std::uint32_t value; std::memcpy(&value, bytes.data()+at, 4); return value; };
    const unsigned sections = u16(offset+6), optional = u16(offset+20), characteristics = u16(offset+22);
    if (u16(offset+4) != BinaryMachine || !sections || sections > 96 || optional < OptionalBytes || !(characteristics & 2)
        || (characteristics & 0x2000) || std::uint64_t(offset) + 24 + optional + 40 * sections > bytes.size()) return false;
    const std::size_t header = offset+24;
    const auto imageSize = u32(header+56), headersSize = u32(header+60), entry = u32(header+16);
    if (u16(header) != OptionalMagic || (u16(header+68) != 2 && u16(header+68) != 3) || !imageSize || !entry
        || entry >= imageSize || headersSize > bytes.size() || headersSize < header + optional + 40 * sections) return false;
    const auto identity = bytes.find(Identity); bool identityInImage = false, executableEntry = false;
    for (unsigned i = 0; i < sections; ++i) {
        const std::size_t section = header + optional + i * 40;
        const auto virtualSize = u32(section+8), rva = u32(section+12), size = u32(section+16), raw = u32(section+20), flags = u32(section+36);
        if (size && (raw < headersSize || std::uint64_t(raw) + size > bytes.size())) return false;
        if (std::uint64_t(rva) + (std::max)(virtualSize, size) > imageSize) return false;
        if (identity >= raw && identity + sizeof(Identity)-1 <= std::uint64_t(raw)+size) identityInImage = true;
        if ((flags & 0x20000000) && entry >= rva && entry < std::uint64_t(rva) + (std::max)(virtualSize, size)) executableEntry = true;
    }
    return identityInImage && executableEntry;
}
Result Apply(const std::filesystem::path& root, const Manifest& manifest, const Policy& policy,
    const std::array<std::filesystem::path, 2>& sources, std::uint64_t installed, unsigned failAfter) {
    Manifest checked; const auto text = document(manifest);
    if (text.empty() || !VerifyManifest(text, policy, installed, &checked)) return failure("Update manifest signature/project/version invalid");
    std::uint64_t actualInstalled = 0;
    if (!installedVersion(root, installed, &actualInstalled) || actualInstalled != installed) return failure("Installed version changed or invalid");
    std::filesystem::path priorJournal;
    if (!path(root, KHZ_UPDATE_DIRECTORY "/transaction.txt", &priorJournal, true)) return failure("Project root invalid or has reparse paths");
    if (GetFileAttributesW(priorJournal.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Journal prior;
        if (!readJournal(root, policy, &prior) || (prior.state != "committed" && prior.state != "rolledback")) return failure("Recover unfinished transaction before new update", true);
    }
    Journal journal; journal.state = "prepared"; journal.previous = installed; journal.manifest = checked;
    std::array<std::filesystem::path, PackagePaths.size()> confinedSources{};
    std::array<std::filesystem::path, InstalledPaths.size()> targets{}, backups{}, replacements{};
    const auto project = std::filesystem::absolute(root).lexically_normal();
    for (unsigned i = 0; i < 2; ++i) {
        std::error_code error; const auto relative = std::filesystem::relative(sources[i], project, error).generic_string();
        if (error || !path(project, relative, &confinedSources[i]) || confinedSources[i] != std::filesystem::absolute(sources[i]).lexically_normal()
            || !matches(confinedSources[i], checked.files[i].sha256, checked.files[i].bytes) || !VerifyBinary(confinedSources[i]))
            return failure("Update source binaries/hash/staging confinement invalid");
    }
    for (unsigned i = 0; i < InstalledPaths.size(); ++i) {
        if (!path(root, InstalledPaths[i], &targets[i], true)) return failure("Installed binary path invalid");
        const DWORD attributes = GetFileAttributesW(targets[i].c_str());
        journal.oldPresent[i] = attributes != INVALID_FILE_ATTRIBUTES;
        if ((!journal.oldPresent[i] && i < PackagePaths.size())
            || (journal.oldPresent[i] && !content::HashFile(targets[i], &journal.oldHashes[i]))
            || !path(root, KHZ_UPDATE_DIRECTORY "/rollback/" + std::to_string(checked.version) + "/" + InstalledPaths[i], &backups[i], true)
            || !path(root, KHZ_UPDATE_DIRECTORY "/staged/" + std::to_string(checked.version) + "/" + InstalledPaths[i], &replacements[i], true))
            return failure("Installed binaries/backup confinement invalid");
    }
    for (unsigned i = 0; i < InstalledPaths.size(); ++i) {
        const unsigned source = InstalledSourceIndexes[i];
        if ((journal.oldPresent[i] && !replaceCopy(targets[i], backups[i], journal.oldHashes[i]))
            || !replaceCopy(confinedSources[source], replacements[i], checked.files[source].sha256))
            return failure("Cannot prepare durable root/runtime backup and replacement set");
    }
    if (!saveJournal(root, journal)) return failure("Cannot persist prepared update journal");
    journal.state = "committing";
    if (!saveJournal(root, journal)) return failure("Cannot persist update commit journal", true);
    for (unsigned i = 0; i < InstalledPaths.size(); ++i) {
        const auto& source = checked.files[InstalledSourceIndexes[i]];
        if (!MoveFileExW(replacements[i].c_str(), targets[i].c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
            || !matches(targets[i], source.sha256, source.bytes) || failAfter == i+1) {
            auto result = rollback(root, journal);
            if (result.rolledBack) std::snprintf(result.detail, sizeof(result.detail), "Update replacement failed; previous root/runtime set restored");
            return result;
        }
    }
    if (!saveVersion(root, checked.version)) return rollback(root, journal);
    journal.state = "committed";
    if (!saveJournal(root, journal)) return failure("Update applied; journal finalization requires recovery", true);
    Result result; result.applied = true; std::snprintf(result.detail, sizeof(result.detail), "Verified root client, runtime server and client mirrors committed; restart required"); return result;
}
RecordedState InspectRecordedState(const std::filesystem::path& root,const Policy& policy) {
    std::filesystem::path file;
    if (!path(root,".khz-inspect-root-check",&file)) return RecordedState::Invalid;
    const DWORD directoryFlags=GetFileAttributesW((root/StorageDirectory).c_str());
    if (directoryFlags==INVALID_FILE_ATTRIBUTES) {
        const auto error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?RecordedState::None:RecordedState::Invalid;
    }
    if (!(directoryFlags&FILE_ATTRIBUTE_DIRECTORY)||(directoryFlags&FILE_ATTRIBUTE_REPARSE_POINT)
        ||!path(root,KHZ_UPDATE_DIRECTORY "/transaction.txt",&file)) return RecordedState::Invalid;
    if (GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)
        return GetLastError()==ERROR_FILE_NOT_FOUND?RecordedState::None:RecordedState::Invalid;
    Journal journal;
    if (!readJournal(root,policy,&journal)) return RecordedState::Invalid;
    if (journal.state=="prepared"||journal.state=="committing") return RecordedState::RecoveryRequired;
    for (unsigned i=0;i<InstalledPaths.size();++i) {
        std::filesystem::path target;
        if (!path(root,InstalledPaths[i],&target)) return RecordedState::RecoveryRequired;
        if (journal.state=="committed") {
            const auto& source=journal.manifest.files[InstalledSourceIndexes[i]];
            if (!matches(target,source.sha256,source.bytes)) return RecordedState::RecoveryRequired;
        } else if (journal.oldPresent[i]) {
            if (!matches(target,journal.oldHashes[i])) return RecordedState::RecoveryRequired;
        } else {
            if (GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES) return RecordedState::RecoveryRequired;
            if (GetLastError()!=ERROR_FILE_NOT_FOUND) return RecordedState::RecoveryRequired;
        }
    }
    std::string text;std::uint64_t version=0;
    if (!path(root,KHZ_UPDATE_DIRECTORY "/version.txt",&file)||!read(file,&text,64)||text.empty()||text.back()!='\n') return RecordedState::RecoveryRequired;
    text.pop_back();
    if (!number(text,&version)||version!=(journal.state=="committed"?journal.manifest.version:journal.previous)) return RecordedState::RecoveryRequired;
    return RecordedState::Healthy;
}
Result Recover(const std::filesystem::path& root, const Policy& policy) {
    std::filesystem::path file;
    if (!path(root, ".khz-root-check", &file)) return failure("Update project path invalid");
    if (GetFileAttributesW((root / StorageDirectory).c_str()) == INVALID_FILE_ATTRIBUTES) { Result result; std::snprintf(result.detail, sizeof(result.detail), "No update transaction"); return result; }
    if (!path(root, KHZ_UPDATE_DIRECTORY "/transaction.txt", &file)) return failure("Update journal path invalid");
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) { Result result; std::snprintf(result.detail, sizeof(result.detail), "No update transaction"); return result; }
    Journal journal;
    if (!readJournal(root, policy, &journal)) return failure("Update journal invalid; refuse game launch", true);
    if (journal.state == "prepared" || journal.state == "committing") return rollback(root, journal);
    bool matchesAll = true;
    for (unsigned i = 0; i < InstalledPaths.size(); ++i) {
        std::filesystem::path target;
        if (!path(root, InstalledPaths[i], &target)) { matchesAll = false; continue; }
        if (journal.state == "committed") {
            const auto& source = journal.manifest.files[InstalledSourceIndexes[i]];
            matchesAll &= matches(target, source.sha256, source.bytes);
        } else if (journal.oldPresent[i]) matchesAll &= matches(target, journal.oldHashes[i]);
        else matchesAll &= GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES;
    }
    if (!matchesAll) return rollback(root, journal);
    if (!saveVersion(root, journal.state == "committed" ? journal.manifest.version : journal.previous)) return failure("Recorded version recovery failed", true);
    Result result; result.applied = journal.state == "committed"; result.rolledBack = journal.state == "rolledback";
    std::snprintf(result.detail, sizeof(result.detail), "Recorded root/runtime binary and mirror state verified"); return result;
}
Result Rollback(const std::filesystem::path& root, const Policy& policy) { Journal journal; if (!readJournal(root, policy, &journal)) return failure("No valid local rollback transaction"); return rollback(root, journal); }
}
