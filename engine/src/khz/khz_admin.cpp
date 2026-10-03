// Power levels, administration aliases and ban durations follow the public
// CoD4x implementation in pinned sv_auth.c/sv_cmds.c/sv_banlist.c. This adapter
// retains upstream license notices in upstream/cod4x_server/COPYING.
#include "khz_admin.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#pragma comment(lib, "bcrypt.lib")
namespace khz::admin {
namespace {
bool equalName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i != a.size(); ++i) {
        const char ac = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
        const char bc = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] + 32) : b[i];
        if (ac != bc) return false;
    }
    return true;
}
bool cleanText(std::string_view value, std::size_t capacity) {
    if (value.size() >= capacity || value.empty()) return false;
    for (unsigned char c : value) if (c < 32 || c == 127) return false;
    return true;
}
template<std::size_t N> void copy(char (&destination)[N], std::string_view value) {
    std::memset(destination, 0, N); std::memcpy(destination, value.data(), value.size());
}
bool passwordHash(std::string_view password, const std::array<std::uint8_t, 32>& salt, std::array<std::uint8_t, 32>& hash) {
    if (password.size() < 8 || password.size() > 128) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
    const bool result = BCryptDeriveKeyPBKDF2(algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), ULONG(password.size()),
        const_cast<PUCHAR>(salt.data()), ULONG(salt.size()), PasswordIterations, hash.data(), ULONG(hash.size()), 0) >= 0;
    BCryptCloseAlgorithmProvider(algorithm, 0); return result;
}
bool sameBan(const BanRecord& ban, const Subject& subject) {
    if (!identity::IsEmpty(subject.identity) && ban.identity == subject.identity) return true;
    if (subject.steamVerified && subject.steamId && ban.steamId == subject.steamId) return true;
    return subject.ipv4 && ban.matchIp && ban.ipv4 == subject.ipv4;
}
bool activeBan(const BanRecord& ban, std::int64_t now) { return ban.active && (!ban.expires || ban.expires > now); }
bool knownSubject(const Subject& subject) {
    return !identity::IsEmpty(subject.identity) || (subject.steamVerified && subject.steamId) || subject.ipv4;
}
bool boundedString(const char* bytes, std::size_t capacity) { return std::memchr(bytes, 0, capacity) != nullptr; }
bool validData(const PersistentData& data) {
    for (unsigned i = 0; i != data.admins.size(); ++i) {
        const auto& admin = data.admins[i];
        if (!boundedString(admin.name, sizeof(admin.name)) || admin.power > 100 || admin.undercover > 1) return false;
        if (admin.power) {
            if (identity::IsEmpty(admin.identity) || !cleanText(admin.name, sizeof(admin.name))) return false;
            for (unsigned j = 0; j < i; ++j) if (data.admins[j].power &&
                (data.admins[j].identity == admin.identity || equalName(data.admins[j].name, admin.name))) return false;
        }
    }
    for (const auto& ban : data.bans) {
        if (ban.active > 1 || ban.matchIp > 1 || !boundedString(ban.name, sizeof(ban.name)) || !boundedString(ban.reason, sizeof(ban.reason))) return false;
        if (ban.active && (ban.created < 0 || (ban.expires && ban.expires <= ban.created) ||
            (!ban.steamId && identity::IsEmpty(ban.identity) && (!ban.matchIp || !ban.ipv4)) ||
            (ban.matchIp && (!ban.ipv4 || !ban.expires || ban.expires - ban.created > 240 * 60)))) return false;
    }
    for (unsigned i = 0; i != data.commands.size(); ++i) {
        const auto& command = data.commands[i];
        if (!boundedString(command.name, sizeof(command.name)) || command.power > 100) return false;
        if (*command.name) {
            char name[64];
            if (!ValidateCommand(command.name, name, sizeof(name)) || std::strcmp(name, command.name)) return false;
            for (unsigned j = 0; j < i; ++j) if (equalName(data.commands[j].name, command.name)) return false;
        }
    }
    return true;
}
struct Header {
    std::uint32_t magic = 0x31414b48, version = 2, bytes = sizeof(PersistentData), reserved = 0; // HKA1
    identity::Fingerprint digest{};
};
static_assert(sizeof(Header) == 48);
struct LegacyPersistentData {
    std::array<AdminRecord, MaxAdmins> admins{};
    std::array<BanRecord, MaxBans> bans{};
    std::array<CommandRule, 256> commands{};
};
}
int Store::Power(const identity::Fingerprint& fingerprint) const {
    if (identity::IsEmpty(fingerprint)) return 0;
    const auto* record = FindAdmin(fingerprint); return record ? record->power : 1;
}
int Store::Power(const Invoker& invoker) const {
    if (invoker.console) return 100;
    if (identity::IsEmpty(invoker.identity)) return 0;
    return invoker.temporaryPower >= 1 && invoker.temporaryPower <= 100 ? invoker.temporaryPower : Power(invoker.identity);
}
const AdminRecord* Store::FindAdmin(const identity::Fingerprint& fingerprint) const {
    if (identity::IsEmpty(fingerprint)) return nullptr;
    for (const auto& record : data_.admins) if (record.power && record.identity == fingerprint) return &record;
    return nullptr;
}
const AdminRecord* Store::FindAdmin(std::string_view name) const {
    for (const auto& record : data_.admins) if (record.power && equalName(record.name, name)) return &record;
    return nullptr;
}
Result Store::SetAdmin(const Invoker& invoker, const identity::Fingerprint& fingerprint, std::string_view name, int power) {
    const int authority = Power(invoker);
    if (authority < 95 || (!invoker.console && invoker.identity == fingerprint) ||
        (authority != 100 && (power >= authority || Power(fingerprint) >= authority))) return Result::Denied;
    if (identity::IsEmpty(fingerprint) || !cleanText(name, 32) || power < 1 || power > 100) return Result::Invalid;
    if (const auto* sameName = FindAdmin(name); sameName && sameName->identity != fingerprint) return Result::Invalid;
    AdminRecord* available = nullptr;
    for (auto& record : data_.admins) {
        if (record.power && record.identity == fingerprint) { available = &record; break; }
        if (!record.power && !available) available = &record;
    }
    if (!available) return Result::Full;
    available->identity = fingerprint; available->power = std::uint8_t(power); copy(available->name, name); WipeSessions(fingerprint);
    return Result::Ok;
}
Result Store::RemoveAdmin(const Invoker& invoker, const identity::Fingerprint& fingerprint) {
    const int authority = Power(invoker);
    if (authority < 95 || (!invoker.console && (invoker.identity == fingerprint || (authority != 100 && Power(fingerprint) >= authority)))) return Result::Denied;
    for (auto& record : data_.admins) if (record.power && record.identity == fingerprint) { record = {}; WipeSessions(fingerprint); return Result::Ok; }
    return Result::NotFound;
}
Result Store::SetAdminWithPassword(const Invoker& invoker, const identity::Fingerprint& fingerprint,
    std::string_view name, int power, std::string_view password) {
    const int authority = Power(invoker);
    if (authority < 95 || (!invoker.console && invoker.identity == fingerprint) ||
        (authority != 100 && (power >= authority || Power(fingerprint) >= authority))) return Result::Denied;
    std::array<std::uint8_t, 32> salt{}, hash{};
    if (!identity::RandomBytes(salt.data(), salt.size()) || !passwordHash(password, salt, hash)) return Result::Invalid;
    const auto result = SetAdmin(invoker, fingerprint, name, power);
    if (result != Result::Ok) return result;
    for (auto& record : data_.admins) if (record.power && record.identity == fingerprint) {
        record.salt = salt; record.passwordHash = hash; return Result::Ok;
    }
    return Result::NotFound;
}
Result Store::SetPassword(const Invoker& invoker, const identity::Fingerprint& fingerprint, std::string_view password) {
    const int authority = Power(invoker);
    const bool own = !invoker.console && invoker.identity == fingerprint && authority >= 10;
    if (!own && (authority < 95 || (authority != 100 && Power(fingerprint) >= authority))) return Result::Denied;
    for (auto& record : data_.admins) if (record.power && record.identity == fingerprint) {
        std::array<std::uint8_t, 32> salt{}, hash{};
        if (!identity::RandomBytes(salt.data(), salt.size()) || !passwordHash(password, salt, hash)) return Result::Invalid;
        record.salt = salt; record.passwordHash = hash; WipeSessions(fingerprint); return Result::Ok;
    }
    return Result::NotFound;
}
Result Store::SetUndercover(const Invoker& invoker, const identity::Fingerprint& fingerprint, bool hidden) {
    if (!invoker.console && invoker.identity != fingerprint) return Result::Denied;
    for (auto& record : data_.admins) if (record.power && record.identity == fingerprint) { record.undercover = hidden ? 1 : 0; return Result::Ok; }
    return Result::NotFound;
}
const AdminRecord* Store::Authorize(std::string_view name, std::string_view password) const {
    const auto* record = FindAdmin(name); if (!record) return nullptr;
    return VerifyCredential(*record, password) ? record : nullptr;
}
bool VerifyCredential(const AdminRecord& record, std::string_view password) {
    const bool configured = std::any_of(record.passwordHash.begin(), record.passwordHash.end(), [](auto value) { return value != 0; });
    std::array<std::uint8_t, 32> hash{};
    if (!record.power || !configured || !passwordHash(password, record.salt, hash)) return false;
    unsigned difference = 0;
    for (unsigned i = 0; i != hash.size(); ++i) difference |= hash[i] ^ record.passwordHash[i];
    SecureZeroMemory(hash.data(), hash.size());
    return difference == 0;
}
const Session* Store::CreateSession(std::string_view name, std::string_view password, std::int64_t now) {
    const auto* admin = Authorize(name, password); if (!admin) return nullptr;
    return AdmitVerifiedCredential(*admin, now);
}
const Session* Store::AdmitVerifiedCredential(const AdminRecord& expected, std::int64_t now) {
    if (now < 0 || now > INT64_MAX - 3600) return nullptr;
    const auto* admin = FindAdmin(expected.identity);
    if (!admin || admin->power != expected.power || admin->salt != expected.salt || admin->passwordHash != expected.passwordHash ||
        std::strcmp(admin->name, expected.name) || !std::any_of(admin->passwordHash.begin(), admin->passwordHash.end(), [](auto value) { return value != 0; })) return nullptr;
    Session* available = nullptr;
    for (auto& session : sessions_) {
        if (session.identity == admin->identity) { available = &session; break; }
        if (session.expires <= now && !available) available = &session;
    }
    if (!available || !identity::RandomBytes(available->token.data(), available->token.size())) return nullptr;
    available->identity = admin->identity; available->expires = now + 3600; return available;
}
const AdminRecord* Store::SessionAdmin(std::string_view tokenHex, std::int64_t now) const {
    std::array<std::uint8_t, 32> decoded{}; if (now < 0 || !identity::DecodeHex(tokenHex, decoded.data(), decoded.size())) return nullptr;
    for (const auto& session : sessions_) {
        unsigned difference = 0; for (unsigned i = 0; i != decoded.size(); ++i) difference |= decoded[i] ^ session.token[i];
        if (!difference && session.expires > now) return FindAdmin(session.identity);
    }
    return nullptr;
}
void Store::WipeSessions(const identity::Fingerprint& fingerprint) { for (auto& session : sessions_) if (session.identity == fingerprint) session = {}; }
Result Store::RegisterCommand(std::string_view name, int defaultPower) {
    char parsed[64];
    if (!ValidateCommand(name, parsed, sizeof(parsed)) || name != parsed || defaultPower < 1 || defaultPower > 100) return Result::Invalid;
    CommandRule* free = nullptr;
    for (auto& command : data_.commands) {
        if (equalName(command.name, name)) return Result::Ok; // Existing persisted override wins.
        if (!*command.name && !free) free = &command;
    }
    if (!free) return Result::Full;
    copy(free->name, name); free->power = std::uint8_t(defaultPower); return Result::Ok;
}
Result Store::SetCommandPower(const Invoker& invoker, std::string_view name, int power) {
    const int authority = Power(invoker);
    if (authority < 98 || (authority != 100 && power >= authority)) return Result::Denied;
    if (power < 1 || power > 100) return Result::Invalid;
    if (equalName(name, "ministatus") && power > 1) return Result::Ok;
    for (auto& command : data_.commands) if (equalName(command.name, name)) { command.power = std::uint8_t(power); return Result::Ok; }
    return Result::NotFound;
}
int Store::CommandPower(std::string_view name) const { for (const auto& command : data_.commands) if (*command.name && equalName(command.name, name)) return command.power; return -1; }
bool Store::CanExecute(const Invoker& invoker, std::string_view command) const {
    char name[64]; if (!ValidateCommand(command, name, sizeof(name))) return false;
    const int needed = CommandPower(name); return needed >= 0 && Power(invoker) >= needed;
}
Result Store::AddBan(const Invoker& invoker, const Subject& subject, std::string_view name, std::string_view reason,
    std::int64_t now, std::int64_t expires, bool matchIp) {
    const int authority = Power(invoker), required = expires ? 50 : 80;
    if (authority < required || (authority != 100 && Power(subject.identity) > authority)) return Result::Denied;
    if (!knownSubject(subject) || (!subject.steamVerified && identity::IsEmpty(subject.identity) && !matchIp) ||
        !cleanText(name, 64) || !cleanText(reason, 127) || now < 0 ||
        (expires && (expires <= now || expires - now > 30 * 24 * 3600)) ||
        (matchIp && (!subject.ipv4 || !expires || expires - now > 240 * 60))) return Result::Invalid;
    BanRecord* available = nullptr;
    for (auto& ban : data_.bans) {
        // NAT peers are not the same identity. Match IP only for IP-only bans.
        if (ban.active && ((!identity::IsEmpty(subject.identity) && ban.identity == subject.identity) ||
            (subject.steamVerified && subject.steamId && ban.steamId == subject.steamId) ||
            (identity::IsEmpty(subject.identity) && !subject.steamId && matchIp && ban.matchIp && ban.ipv4 == subject.ipv4))) { available = &ban; break; }
        if (!activeBan(ban, now) && !available) available = &ban;
    }
    if (!available) return Result::Full;
    *available = {}; available->identity = subject.identity; available->steamId = subject.steamVerified ? subject.steamId : 0;
    available->issuer = invoker.identity; available->created = now; available->expires = expires;
    available->ipv4 = subject.ipv4; available->matchIp = matchIp ? 1 : 0; available->active = 1;
    copy(available->name, name); copy(available->reason, reason); return Result::Ok;
}
Result Store::RemoveBan(const Invoker& invoker, const Subject& subject) {
    if (Power(invoker) < 80) return Result::Denied;
    bool found = false;
    for (auto& ban : data_.bans) if (ban.active && sameBan(ban, subject)) { ban = {}; found = true; }
    return found ? Result::Ok : Result::NotFound;
}
const BanRecord* Store::IsBanned(const Subject& subject, std::int64_t now) const {
    for (const auto& ban : data_.bans) if (activeBan(ban, now) && sameBan(ban, subject)) return &ban;
    return nullptr;
}
void Store::ExpireBans(std::int64_t now) { for (auto& ban : data_.bans) if (ban.active && ban.expires && ban.expires <= now) ban = {}; }
bool Store::Load(const wchar_t* filename) {
    HANDLE file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Header header{}; DWORD read = 0; LARGE_INTEGER length{};
    bool complete = GetFileSizeEx(file, &length) && ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header) &&
        header.magic == Header{}.magic && !header.reserved &&
        ((header.version == 2 && header.bytes == sizeof(PersistentData)) ||
         (header.version == 1 && header.bytes == sizeof(LegacyPersistentData))) &&
        length.QuadPart == sizeof(Header) + header.bytes;
    auto pending = std::make_unique<PersistentData>();
    auto legacy = header.version == 1 ? std::make_unique<LegacyPersistentData>() : nullptr;
    void* payload = legacy ? static_cast<void*>(legacy.get()) : static_cast<void*>(pending.get());
    if (complete) complete = ReadFile(file, payload, header.bytes, &read, nullptr) && read == header.bytes;
    CloseHandle(file); identity::Fingerprint digest{};
    if (!complete || !identity::Sha256(payload, header.bytes, digest) || digest != header.digest) return false;
    if (legacy) {
        pending->admins = legacy->admins; pending->bans = legacy->bans;
        std::copy(legacy->commands.begin(), legacy->commands.end(), pending->commands.begin());
    }
    if (!validData(*pending)) return false;
    data_ = *pending; sessions_ = {}; return true;
}
bool Store::Save(const wchar_t* filename) const {
    if (!filename || !*filename || !validData(data_)) return false;
    Header header{}; if (!identity::Sha256(&data_, sizeof(data_), header.digest)) return false;
    wchar_t temporary[32768]{}, backup[32768]{};
    static LONG serial = 0;
    if (_snwprintf_s(temporary, _countof(temporary), _TRUNCATE, L"%s.tmp.%lu.%ld", filename, GetCurrentProcessId(), InterlockedIncrement(&serial)) < 0 ||
        _snwprintf_s(backup, _countof(backup), _TRUNCATE, L"%s.previous", filename) < 0) return false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool success = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
        WriteFile(file, &data_, sizeof(data_), &written, nullptr) && written == sizeof(data_) && FlushFileBuffers(file);
    CloseHandle(file);
    if (success) {
        if (GetFileAttributesW(filename) != INVALID_FILE_ATTRIBUTES) success = ReplaceFileW(filename, temporary, backup, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE;
        else success = MoveFileExW(temporary, filename, MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!success) DeleteFileW(temporary); return success;
}
unsigned Store::AdminCount() const { unsigned count = 0; for (const auto& admin : data_.admins) if (admin.power) ++count; return count; }
unsigned Store::BanCount(std::int64_t now) const { unsigned count = 0; for (const auto& ban : data_.bans) if (activeBan(ban, now)) ++count; return count; }
bool ValidateCommand(std::string_view command, char* name, std::size_t capacity) {
    if (!name || !capacity || command.empty() || command.size() > 1023) return false;
    name[0] = 0;
    bool quoted = false;
    for (unsigned char c : command) {
        if (c == ';' || c < 32 || c == 127) return false;
        if (c == '"') quoted = !quoted;
    }
    if (quoted) return false;
    while (!command.empty() && command.front() == ' ') command.remove_prefix(1);
    const auto length = command.find(' '); const auto token = command.substr(0, length);
    if (token.empty() || token.size() >= capacity) return false;
    for (unsigned char c : token) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    std::memcpy(name, token.data(), token.size()); name[token.size()] = 0; return true;
}
bool SameAdmission(const Admission& expected, const Admission& current) {
    return expected.generation && expected.generation == current.generation &&
        !identity::IsEmpty(expected.identity) && expected.identity == current.identity;
}
bool CommandWhitelist::Add(std::string_view command) {
    char name[64];
    if (!ValidateCommand(command, name, sizeof(name)) || command != name) return false;
    for (auto& entry : commands_) if (*entry.data() && equalName(entry.data(), name)) return true;
    for (auto& entry : commands_) if (!*entry.data()) {
        std::memcpy(entry.data(), name, std::strlen(name) + 1); return true;
    }
    return false;
}
bool CommandWhitelist::Contains(std::string_view command) const {
    for (const auto& entry : commands_) if (*entry.data() && equalName(entry.data(), command)) return true;
    return false;
}
bool CanExecute(const Store& store, const Invoker& invoker, std::string_view command,
    const CommandWhitelist* whitelist) {
    char name[64]; if (!ValidateCommand(command, name, sizeof(name))) return false;
    const int required = store.CommandPower(name);
    if (required < 0 || store.Power(invoker) == 0) return false;
    return store.Power(invoker) >= required || (whitelist && whitelist->Contains(name));
}
void WriteBanTimelimit(std::int64_t remainingSeconds, char* text, std::size_t capacity) {
    if (!text || !capacity) return;
    if (remainingSeconds < 0) std::snprintf(text, capacity, "This is a permanent ban.\n");
    else {
        const auto remaining = remainingSeconds > INT64_MAX - 60 ? INT64_MAX : remainingSeconds + 60;
        std::snprintf(text, capacity, "This is a temporary ban and will expire in\n %lld days %lld hours %lld minutes\n",
            remaining / 86400, remaining / 3600 % 24, remaining / 60 % 60);
    }
    text[capacity - 1] = 0;
}
void FormatBanMessage(std::int64_t remainingSeconds, const char* reason, const char* appealUrl,
    unsigned appealMinimumHours, char* text, std::size_t capacity) {
    if (!text || !capacity) return;
    const bool appeal = appealUrl && *appealUrl && (remainingSeconds < 0 ||
        remainingSeconds >= std::int64_t(appealMinimumHours) * 3600);
    std::snprintf(text, capacity, "You have been banned from this server with the following reason: %s\n%s%s",
        reason ? reason : "", appeal ? appealUrl : "", appeal ? "\n" : "");
    text[capacity - 1] = 0;
}
bool ParseBanDuration(std::string_view text, std::int64_t& seconds) {
    seconds = 0; if (text.size() < 2 || text.size() > 7) return false;
    std::int64_t scale;
    switch (text.back()) { case 'm': scale = 60; break; case 'h': scale = 3600; break; case 'd': scale = 86400; break; default: return false; }
    text.remove_suffix(1); std::int64_t value = 0;
    for (char c : text) { if (c < '0' || c > '9') return false; value = value * 10 + c - '0'; }
    seconds = value * scale; return seconds >= 60 && seconds <= 30 * 86400;
}
bool SafeRotationToken(std::string_view token) {
    if (token.empty() || token.size() >= 64) return false;
    for (unsigned char c : token) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
bool CanUseReservedSlots(int power, bool passwordMatches, int minimumPower) { return passwordMatches || (minimumPower >= 1 && minimumPower <= 100 && power >= minimumPower); }
bool ParseRotation(std::string_view text, std::string_view initialGametype, Rotation& rotation) {
    rotation = {}; if (text.size() > 8191 || !SafeRotationToken(initialGametype)) return false;
    char gametype[64]; copy(gametype, initialGametype);
    auto token = [&]() {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
        const auto end = text.find_first_of(" \t\r\n"); const auto result = text.substr(0, end);
        if (end == std::string_view::npos) text = {}; else text.remove_prefix(end);
        return result;
    };
    while (true) {
        const auto keyword = token(); if (keyword.empty()) break;
        const auto value = token(); if (!SafeRotationToken(value)) { rotation = {}; return false; }
        if (equalName(keyword, "gametype")) copy(gametype, value);
        else if (equalName(keyword, "map")) {
            if (rotation.count == rotation.maps.size()) { rotation = {}; return false; }
            auto& entry = rotation.maps[rotation.count++]; copy(entry.map, value); copy(entry.gametype, gametype);
        } else { rotation = {}; return false; }
    }
    return rotation.count != 0;
}
bool ShuffleRotation(Rotation& rotation) {
    if (!rotation.count || rotation.count > rotation.maps.size()) return false;
    for (unsigned i = rotation.count - 1; i > 0; --i) {
        const std::uint64_t range = i + 1, limit = (0x100000000ULL / range) * range;
        std::uint32_t random;
        do { if (!identity::RandomBytes(&random, sizeof(random))) return false; } while (random >= limit);
        std::swap(rotation.maps[i], rotation.maps[random % range]);
    }
    return true;
}
bool SerializeRotation(const Rotation& rotation, unsigned first, char* text, std::size_t capacity) {
    if (!text || !capacity || rotation.count > rotation.maps.size() || first > rotation.count) return false;
    text[0] = 0; std::size_t used = 0;
    for (unsigned i = first; i < rotation.count; ++i) {
        const auto& entry = rotation.maps[i];
        if (!boundedString(entry.map, sizeof(entry.map)) || !boundedString(entry.gametype, sizeof(entry.gametype)) ||
            !SafeRotationToken(entry.map) || !SafeRotationToken(entry.gametype)) return false;
        const int length = std::snprintf(text + used, capacity - used, "%sgametype %s map %s", used ? " " : "", entry.gametype, entry.map);
        if (length < 0 || std::size_t(length) >= capacity - used) { text[0] = 0; return false; }
        used += length;
    }
    return true;
}
bool IpBanCache::Add(std::uint32_t address, std::string_view reason, std::int64_t now,
    std::int64_t requestedExpiry, unsigned maximumMinutes) {
    if (!address || !maximumMinutes || maximumMinutes > 240 || now < 0 || now > INT64_MAX - maximumMinutes * 60 ||
        !cleanText(reason, 256) || (requestedExpiry && requestedExpiry <= now)) return false;
    auto expiry = now + maximumMinutes * 60;
    if (requestedExpiry) expiry = std::min(expiry, requestedExpiry);
    Entry* available = &entries_[0];
    for (auto& entry : entries_) {
        if (entry.address == address) { available = &entry; break; }
        if (!entry.address || entry.expires <= now) available = &entry;
        else if (available->address && available->expires > now && entry.added < available->added) available = &entry;
    }
    *available = {}; available->address = address; available->added = now; available->expires = expiry; copy(available->reason, reason); return true;
}
const char* IpBanCache::Find(std::uint32_t address, std::int64_t now) const {
    if (!address || now < 0) return nullptr;
    for (const auto& entry : entries_) if (entry.address == address && entry.expires > now) return entry.reason;
    return nullptr;
}
bool IpBanCache::Format(std::uint32_t address, std::int64_t now, char* message, std::size_t capacity) const {
    if (!message || !capacity) return false; message[0] = 0;
    if (!address || now < 0) return false;
    for (const auto& entry : entries_) if (entry.address == address && entry.expires > now && entry.expires - now > 1) {
        char time[512]; WriteBanTimelimit(entry.expires - now, time, sizeof(time));
        std::snprintf(message, capacity, "%s\n%s\n", entry.reason, time); return true;
    }
    return false;
}
void IpBanCache::Remove(std::uint32_t address) { for (auto& entry : entries_) if (entry.address == address) entry = {}; }
bool ConstantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() > 1024 || b.size() > 1024) return false;
    std::size_t difference = a.size() ^ b.size(); const auto maximum = std::max(a.size(), b.size());
    for (std::size_t i = 0; i != maximum; ++i) difference |= (i < a.size() ? std::uint8_t(a[i]) : 0) ^ (i < b.size() ? std::uint8_t(b[i]) : 0);
    return difference == 0;
}
RconResult RconLimiter::Validate(std::uint32_t address, std::uint64_t now, std::string_view supplied, std::string_view configured) {
    auto take = [now](Bucket& bucket, unsigned burst, unsigned period) {
        if (!bucket.occupied || now < bucket.updated) { bucket.occupied = true; bucket.updated = now; bucket.used = 0; }
        const auto elapsed = now - bucket.updated;
        if (elapsed >= period) { const auto drained = elapsed / period; bucket.used = drained >= bucket.used ? 0 : bucket.used - drained; bucket.updated += drained * period; }
        if (bucket.used >= burst) return false;
        ++bucket.used; return true;
    };
    Bucket* bucket = nullptr; Bucket* oldest = &addresses_[0];
    for (auto& candidate : addresses_) {
        if (candidate.occupied && candidate.address == address) { bucket = &candidate; break; }
        if (!candidate.occupied) oldest = &candidate;
        else if (oldest->occupied && candidate.updated < oldest->updated) oldest = &candidate;
    }
    if (!bucket) { bucket = oldest; *bucket = {}; bucket->address = address; }
    // Per-source bound applies before password comparison. Bad-password replies
    // additionally share the upstream 1/100ms global limit. Good sources do not
    // consume that bucket, so a bad source cannot starve authenticated RCON.
    if (!take(*bucket, 8, 100)) return RconResult::Limited;
    if (configured.size() < 8 || configured.size() > 128) return take(globalBad_, 1, 100) ? RconResult::Disabled : RconResult::Limited;
    if (!ConstantTimeEqual(supplied, configured)) return take(globalBad_, 1, 100) ? RconResult::BadPassword : RconResult::Limited;
    return take(globalGood_, 16, 10) ? RconResult::Accepted : RconResult::Limited;
}
}
