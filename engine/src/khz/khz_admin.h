#pragma once
#include "khz_identity.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Bounded policy/data layer. Runtime adapters supply the actual invoker and
// verified identities. Disk, password derivation and log flushing are service
// work, never part of player movement or snapshot serialization.
namespace khz::admin {
inline constexpr unsigned MaxAdmins = 512;
inline constexpr unsigned MaxBans = 1024;
inline constexpr unsigned MaxCommandRules = 512; // 320 plugin commands plus engine/administration commands
inline constexpr unsigned MaxSessions = 64;
inline constexpr unsigned PasswordIterations = 150000;
// A temporary override is supplied only by trusted server script/plugin code.
// Wire clients never choose it and an absent verified fingerprint cannot use it.
struct Invoker { identity::Fingerprint identity{}; bool console = false; int temporaryPower = -1; };
struct AdminRecord {
    identity::Fingerprint identity{};
    std::array<std::uint8_t, 32> salt{}, passwordHash{};
    char name[32]{};
    std::uint8_t power = 0, undercover = 0;
};
struct Subject {
    identity::Fingerprint identity{};
    std::uint64_t steamId = 0;
    std::uint32_t ipv4 = 0;
    bool steamVerified = false;
};
// A slot number is only a presentation handle. Deferred actions retain this
// admission token and a copied subject so slot reuse cannot change their target.
struct Admission { identity::Fingerprint identity{}; std::uint64_t generation = 0; };
bool SameAdmission(const Admission& expected, const Admission& current);
class CommandWhitelist {
public:
    bool Add(std::string_view command);
    bool Contains(std::string_view command) const;
    void Clear() { commands_ = {}; }
private:
    std::array<std::array<char, 64>, 32> commands_{};
};
struct BanRecord {
    identity::Fingerprint identity{}, issuer{};
    std::uint64_t steamId = 0;
    std::int64_t created = 0, expires = 0; // Unix seconds, zero is permanent.
    std::uint32_t ipv4 = 0;
    std::uint8_t matchIp = 0, active = 0;
    char name[64]{}, reason[127]{};
};
struct CommandRule { char name[64]{}; std::uint8_t power = 0; };
struct Session {
    std::array<std::uint8_t, 32> token{};
    identity::Fingerprint identity{};
    std::int64_t expires = 0;
};
struct PersistentData {
    std::array<AdminRecord, MaxAdmins> admins{};
    std::array<BanRecord, MaxBans> bans{};
    std::array<CommandRule, MaxCommandRules> commands{};
};
enum class Result { Ok, Denied, Invalid, Full, NotFound, StorageError };

class Store {
public:
    int Power(const identity::Fingerprint& identity) const;
    int Power(const Invoker& invoker) const;
    const AdminRecord* FindAdmin(const identity::Fingerprint& identity) const;
    const AdminRecord* FindAdmin(std::string_view name) const;
    Result SetAdmin(const Invoker& invoker, const identity::Fingerprint& identity, std::string_view name, int power);
    Result SetAdminWithPassword(const Invoker& invoker, const identity::Fingerprint& identity,
        std::string_view name, int power, std::string_view password);
    Result RemoveAdmin(const Invoker& invoker, const identity::Fingerprint& identity);
    Result SetPassword(const Invoker& invoker, const identity::Fingerprint& identity, std::string_view password);
    Result SetUndercover(const Invoker& invoker, const identity::Fingerprint& identity, bool hidden);
    const AdminRecord* Authorize(std::string_view name, std::string_view password) const;
    const Session* CreateSession(std::string_view name, std::string_view password, std::int64_t now);
    // Trusted worker completion only: caller verified this copied record's
    // password off-thread. The owner still rejects changed/revoked credentials.
    const Session* AdmitVerifiedCredential(const AdminRecord& expected, std::int64_t now);
    const AdminRecord* SessionAdmin(std::string_view tokenHex, std::int64_t now) const;
    void WipeSessions(const identity::Fingerprint& identity);
    Result RegisterCommand(std::string_view name, int defaultPower);
    Result SetCommandPower(const Invoker& invoker, std::string_view name, int power);
    int CommandPower(std::string_view name) const;
    bool CanExecute(const Invoker& invoker, std::string_view command) const;
    Result AddBan(const Invoker& invoker, const Subject& subject, std::string_view name,
        std::string_view reason, std::int64_t now, std::int64_t expires, bool matchIp);
    Result RemoveBan(const Invoker& invoker, const Subject& subject);
    const BanRecord* IsBanned(const Subject& subject, std::int64_t now) const;
    void ExpireBans(std::int64_t now);
    bool Load(const wchar_t* filename);
    bool Save(const wchar_t* filename) const;
    const PersistentData& Data() const { return data_; }
    unsigned AdminCount() const;
    unsigned BanCount(std::int64_t now) const;
private:
    PersistentData data_{};
    std::array<Session, MaxSessions> sessions_{};
};

// Exactly one command for an in-game admin request. Unlike trusted console
// buffers, embedded separators/control bytes are rejected, not truncated.
bool ValidateCommand(std::string_view command, char* name, std::size_t capacity);
// Whitelists bypass the named command's entry power only. The Store still
// enforces target hierarchy and mutation permissions inside administration.
bool CanExecute(const Store& store, const Invoker& invoker, std::string_view command,
    const CommandWhitelist* whitelist);
bool ParseBanDuration(std::string_view text, std::int64_t& seconds);
void WriteBanTimelimit(std::int64_t remainingSeconds, char* text, std::size_t capacity);
void FormatBanMessage(std::int64_t remainingSeconds, const char* reason, const char* appealUrl,
    unsigned appealMinimumHours, char* text, std::size_t capacity);
bool SafeRotationToken(std::string_view token);
bool CanUseReservedSlots(int power, bool passwordMatches, int minimumPower);

struct RotationMap { char map[64]{}, gametype[64]{}; };
struct Rotation { std::array<RotationMap, 64> maps{}; unsigned count = 0; };
bool ParseRotation(std::string_view text, std::string_view initialGametype, Rotation& rotation);
bool ShuffleRotation(Rotation& rotation);
bool SerializeRotation(const Rotation& rotation, unsigned first, char* text, std::size_t capacity);

// The public32-entry short IP-ban cache is independent of persistent identity
// bans. Expiries use real seconds and its configurable NAT bound defaults4h.
class IpBanCache {
public:
    bool Add(std::uint32_t address, std::string_view reason, std::int64_t now,
        std::int64_t requestedExpiry, unsigned maximumMinutes = 240);
    const char* Find(std::uint32_t address, std::int64_t now) const;
    bool Format(std::uint32_t address, std::int64_t now, char* message, std::size_t capacity) const;
    void Remove(std::uint32_t address);
private:
    struct Entry { std::uint32_t address = 0; std::int64_t added = 0, expires = 0; char reason[256]{}; };
    std::array<Entry, 32> entries_{};
};

enum class RconResult { Accepted, BadPassword, Disabled, Limited };
class RconLimiter {
public:
    RconResult Validate(std::uint32_t address, std::uint64_t nowMs, std::string_view supplied, std::string_view configured);
private:
    struct Bucket { std::uint32_t address = 0; std::uint64_t updated = 0, used = 0; bool occupied = false; };
    std::array<Bucket, 128> addresses_{};
    Bucket globalBad_{}, globalGood_{};
};
bool ConstantTimeEqual(std::string_view a, std::string_view b);
bool VerifyCredential(const AdminRecord& record, std::string_view password);
}
