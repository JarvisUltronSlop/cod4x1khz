#include "khz_identity.h"
#include "khz_admin.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
namespace id = khz::identity;
namespace adm = khz::admin;
id::Fingerprint fingerprint(unsigned value) {
    id::Fingerprint result{}; result[0] = std::uint8_t(value); result[1] = std::uint8_t(value >> 8); result[2] = 1; return result;
}
struct LegacyPolicyPayload {
    std::array<adm::AdminRecord, adm::MaxAdmins> admins{};
    std::array<adm::BanRecord, adm::MaxBans> bans{};
    std::array<adm::CommandRule, 256> commands{};
};
struct LegacyPolicyHeader {
    std::uint32_t magic = 0x31414b48, version = 1, bytes = sizeof(LegacyPolicyPayload), reserved = 0;
    id::Fingerprint digest{};
};
int main() {
    CHECK(id::GameChallengeNumber(0) == 1); CHECK(id::GameChallengeNumber(0x80000000u) == 1);
    CHECK(id::GameChallengeNumber(0xffffffffu) == 0x7fffffffu); CHECK(id::GameChallengeNumber(123) == 123);
    id::Fingerprint digest{}; char hex[273];
    CHECK(id::Sha256("abc", 3, digest)); CHECK(id::EncodeHex(digest.data(), digest.size(), hex, sizeof(hex)));
    CHECK(std::strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    CHECK(!id::DecodeHex("zz", digest.data(), 1)); CHECK(!id::DecodeHex("00\n", digest.data(), 1));
    id::PrivateKey key; CHECK(id::GenerateKey(key)); id::PublicKey publicKey{}; CHECK(id::ExportPublicKey(key, publicKey));
    id::Challenge challenge{}; challenge.challenge = 45; challenge.qport = 28961; challenge.protocol = 10002;
    CHECK(id::RandomBytes(challenge.boot.data(), challenge.boot.size())); CHECK(id::RandomBytes(challenge.nonce.data(), challenge.nonce.size()));
    id::Proof proof{}; CHECK(id::Sign(key, challenge, proof)); CHECK(id::Verify(proof, challenge, digest));
    const auto originalFingerprint = digest; CHECK(id::EncodeProof(proof, hex, sizeof(hex)));
    id::Proof decoded{}; CHECK(id::DecodeProof(hex, decoded)); CHECK(id::Verify(decoded, challenge, digest)); CHECK(digest == originalFingerprint);
    CHECK(!id::DecodeProof(std::string_view(hex, sizeof(hex) - 2), decoded));
    auto changed = challenge; ++changed.protocol; CHECK(!id::Verify(proof, changed, digest)); CHECK(id::IsEmpty(digest));
    changed = challenge; ++changed.challenge; CHECK(!id::Verify(proof, changed, digest));
    changed = challenge; ++changed.qport; CHECK(!id::Verify(proof, changed, digest));
    changed = challenge; changed.nonce[4] ^= 1; CHECK(!id::Verify(proof, changed, digest));
    changed = challenge; changed.boot[5] ^= 1; CHECK(!id::Verify(proof, changed, digest));
    auto tampered = proof; tampered.signature[9] ^= 1; CHECK(!id::Verify(tampered, challenge, digest));
    tampered = proof; tampered.publicKey[0] = 0; CHECK(!id::Verify(tampered, challenge, digest));
    id::PrivateKey otherKey; CHECK(id::GenerateKey(otherKey)); CHECK(id::ExportPublicKey(otherKey, tampered.publicKey));
    tampered.signature = proof.signature; CHECK(!id::Verify(tampered, challenge, digest));
    const auto fixture = std::filesystem::current_path() / "identity-admin-fixtures";
    std::filesystem::create_directories(fixture);
    const auto keyPath = fixture / ("identity-" + std::to_string(GetCurrentProcessId()) + ".bin");
    CHECK(id::LoadOrCreateKey(keyPath.c_str(), key)); CHECK(id::ExportPublicKey(key, publicKey));
    CHECK(id::LoadOrCreateKey(keyPath.c_str(), otherKey)); id::PublicKey reloaded{}; CHECK(id::ExportPublicKey(otherKey, reloaded)); CHECK(publicKey == reloaded);
    // DPAPI metadata fields are not all authenticated plaintext. Flip an
    // actual byte at the end of the protected payload, rather than assigning a
    // fixed value to a header offset which may already contain that value.
    { std::fstream file(keyPath, std::ios::binary | std::ios::in | std::ios::out); CHECK(file);
      file.seekg(-1, std::ios::end); char byte = 0; file.get(byte); CHECK(file);
      file.seekp(-1, std::ios::end); file.put(char(static_cast<unsigned char>(byte) ^ 1)); }
    CHECK(!id::LoadOrCreateKey(keyPath.c_str(), otherKey)); // corrupt identity never regenerates silently
    std::filesystem::remove(keyPath);
    constexpr std::uint64_t steam = (1ULL << 56) | (1ULL << 52) | (1ULL << 32) | 12345;
    CHECK(id::StringToSteamId("[U:1:12345]") == steam); CHECK(id::StringToSteamId(" U:1:12345 ") == steam);
    CHECK(id::StringToSteamId("[U:1:12345:1]") == steam);
    CHECK(id::StringToSteamId("STEAM_1:1:6172") == steam);
    CHECK(id::SteamIdToString(steam, true, hex, sizeof(hex))); CHECK(id::StringToSteamId(hex) == steam);
    CHECK(id::SteamIdToString(steam, false, hex, sizeof(hex))); CHECK(std::strcmp(hex, "[U:1:12345]") == 0);
    CHECK(id::SteamIdIndividual(steam, true)); CHECK(!id::SteamIdIndividual((32ULL << 56) | (1ULL << 52) | (1ULL << 32) | 123, true));
    CHECK(id::SteamIdIndividual((32ULL << 56) | (1ULL << 52) | (1ULL << 32) | 123));
    for (const char* bad : {"", "[]", "[U:0:1]", "[U:40:1]", "[U:1:4294967296]", "[U:1:1:1048576]", "[U:1:1]junk", "18446744073709551616", "STEAM_9:0:1", "STEAM_1:2:1"}) CHECK(id::StringToSteamId(bad) == 0);
    // Independent .NET PBKDF2-SHA256 derivation from the pinned 256-byte salt.
    const auto guidId = id::GuidToPlayerId("0123456789abcdef0123456789abcdef"); CHECK(guidId == 2310346614158149229ULL); CHECK(id::SteamIdIndividual(guidId));
    CHECK(id::GuidToPlayerId("0123456789ABCDEF0123456789ABCDEF") == guidId); CHECK(!id::GuidToPlayerId("invalid"));

    auto store = std::make_unique<adm::Store>(); const adm::Invoker console{{}, true};
    const auto master = fingerprint(1), senior = fingerprint(2), moderator = fingerprint(3), player = fingerprint(4);
    CHECK(store->SetAdmin(console, master, "Master", 100) == adm::Result::Ok);
    CHECK(store->SetAdmin(console, senior, "Senior", 95) == adm::Result::Ok);
    CHECK(store->SetAdmin(console, moderator, "Moderator", 50) == adm::Result::Ok);
    CHECK(store->SetAdmin(console, player, "master", 5) == adm::Result::Invalid); // username collision
    CHECK(store->SetAdminWithPassword(console, fingerprint(19), "Rejected", 10, "short") == adm::Result::Invalid);
    CHECK(!store->FindAdmin(fingerprint(19))); // no partial account after failed password setup
    CHECK(store->SetAdmin({senior}, player, "Player", 95) == adm::Result::Denied);
    CHECK(store->SetAdmin({moderator}, player, "Player", 1) == adm::Result::Denied);
    CHECK(store->SetAdmin({senior}, master, "Master", 5) == adm::Result::Denied);
    CHECK(store->RemoveAdmin({senior}, master) == adm::Result::Denied);
    CHECK(store->SetAdmin({senior}, player, "Player", 10) == adm::Result::Ok);
    CHECK(store->Power(player) == 10); CHECK(store->Power(fingerprint(9999)) == 1); CHECK(store->Power(id::Fingerprint{}) == 0);
    CHECK(store->RegisterCommand("tempban", 50) == adm::Result::Ok); CHECK(store->RegisterCommand("map", 60) == adm::Result::Ok);
    CHECK(store->CanExecute({moderator}, "tempban 5 2m \"reason with spaces\""));
    CHECK(!store->CanExecute({moderator}, "map mp_crash")); CHECK(!store->CanExecute({master}, "map mp_crash; quit"));
    CHECK(!store->CanExecute({master}, "map mp_crash\nquit")); CHECK(!store->CanExecute({master}, "set rcon_password stolen"));
    CHECK(store->SetCommandPower({senior}, "map", 1) == adm::Result::Denied);
    CHECK(store->SetCommandPower(console, "map", 40) == adm::Result::Ok); CHECK(store->CanExecute({moderator}, "map mp_crash"));
    CHECK(store->RegisterCommand("ministatus", 1) == adm::Result::Ok);
    CHECK(store->SetCommandPower(console, "ministatus", 100) == adm::Result::Ok); CHECK(store->CommandPower("ministatus") == 1);
    CHECK(store->SetCommandPower(console, "map", 0) == adm::Result::Invalid);
    adm::CommandWhitelist whitelist;
    CHECK(whitelist.Add("map")); CHECK(whitelist.Add("MAP")); // duplicate is idempotent
    CHECK(!whitelist.Add("map mp_crash")); CHECK(!whitelist.Add("map;quit"));
    CHECK(!adm::CanExecute(*store, {player}, "map mp_crash", nullptr));
    CHECK(adm::CanExecute(*store, {player}, "map mp_crash", &whitelist));
    CHECK(!adm::CanExecute(*store, {}, "map mp_crash", &whitelist)); // no verified identity
    CHECK(!adm::CanExecute(*store, {player}, "map mp_crash; quit", &whitelist));
    CHECK(whitelist.Add("unknown")); CHECK(!adm::CanExecute(*store, {player}, "unknown", &whitelist));
    CHECK(store->SetCommandPower(console, "map", 100) == adm::Result::Ok);
    CHECK(!adm::CanExecute(*store, {moderator}, "map mp_crash", nullptr)); // live rule, not registration-time40
    CHECK(adm::CanExecute(*store, {player}, "map mp_crash", &whitelist));
    whitelist.Clear(); CHECK(!adm::CanExecute(*store, {player}, "map mp_crash", &whitelist));
    for (unsigned i = 0; i != 32; ++i) CHECK(whitelist.Add("fixture_" + std::to_string(i)));
    CHECK(!whitelist.Add("thirtythird")); CHECK(whitelist.Add("fixture_0"));
    CHECK(store->SetPassword({moderator}, master, "password-123") == adm::Result::Denied);
    CHECK(store->SetPassword(console, master, "short") == adm::Result::Invalid);
    CHECK(store->SetPassword(console, master, "password-123") == adm::Result::Ok);
    CHECK(store->Authorize("master", "password-123")); CHECK(!store->Authorize("Master", "wrong-password"));
    const auto* session = store->CreateSession("MASTER", "password-123", 1000); CHECK(session);
    char sessionToken[65]; CHECK(id::EncodeHex(session->token.data(), session->token.size(), sessionToken, sizeof(sessionToken)));
    CHECK(store->SessionAdmin(sessionToken, 1001)); CHECK(!store->SessionAdmin(sessionToken, 4600));
    CHECK(store->SetPassword(console, master, "replacement-123") == adm::Result::Ok); CHECK(!store->SessionAdmin(sessionToken, 1001));
    adm::Subject subject{fingerprint(20), 0, 0x0100007f, false};
    const adm::Admission originalAdmission{subject.identity, 7};
    CHECK(adm::SameAdmission(originalAdmission, originalAdmission));
    CHECK(!adm::SameAdmission(originalAdmission, {subject.identity, 8})); // same key reconnect
    CHECK(!adm::SameAdmission(originalAdmission, {fingerprint(21), 7})); // replaced slot
    CHECK(!adm::SameAdmission({}, {}));
    const auto queuedSubject = subject; // copied before service/slot reuse
    subject.identity = fingerprint(22);
    CHECK(store->AddBan(console, queuedSubject, "Original", "Queued reason", 1000, 1060, false) == adm::Result::Ok);
    CHECK(store->IsBanned(queuedSubject, 1001)); CHECK(!store->IsBanned(subject, 1001));
    CHECK(store->RemoveBan(console, queuedSubject) == adm::Result::Ok); subject = queuedSubject;
    CHECK(store->AddBan({moderator}, subject, "Test", "Reason", 1000, 1060, false) == adm::Result::Ok);
    CHECK(store->IsBanned(subject, 1059)); CHECK(!store->IsBanned(subject, 1060));
    CHECK(store->BanCount(1060) == 0); store->ExpireBans(1060);
    CHECK(store->AddBan({moderator}, subject, "Test", "Reason", 1000, 0, false) == adm::Result::Denied);
    CHECK(store->AddBan(console, subject, "Test", "Reason", 1000, 0, false) == adm::Result::Ok);
    const adm::Subject natPeer{fingerprint(21), 0, subject.ipv4, false}; CHECK(!store->IsBanned(natPeer, 1059));
    CHECK(store->RemoveBan({moderator}, subject) == adm::Result::Denied);
    CHECK(store->RemoveBan(console, subject) == adm::Result::Ok);
    CHECK(store->AddBan(console, subject, "Test", "Reason", 1000, 1600, true) == adm::Result::Ok);
    CHECK(store->IsBanned(natPeer, 1001)); CHECK(!store->IsBanned(natPeer, 1600));
    CHECK(store->AddBan(console, subject, "Test", "Reason", 1000, 0, true) == adm::Result::Invalid);
    CHECK(store->AddBan(console, subject, "Test", "Reason", 1000, 1000 + 240 * 60 + 1, true) == adm::Result::Invalid);
    CHECK(store->AddBan(console, subject, "Test", "Reason", 1000, 1000 + 30 * 86400 + 1, false) == adm::Result::Invalid);
    adm::Subject spoofed{{}, steam, 0, false}; CHECK(store->AddBan(console, spoofed, "Test", "Reason", 1000, 0, false) == adm::Result::Invalid);
    spoofed.steamVerified = true; CHECK(store->AddBan(console, spoofed, "Test", "Reason", 1000, 0, false) == adm::Result::Ok);
    spoofed.steamVerified = false; CHECK(!store->IsBanned(spoofed, 1001));
    adm::Subject protectedAdmin{master}; CHECK(store->AddBan({moderator}, protectedAdmin, "Master", "Reason", 1000, 1060, false) == adm::Result::Denied);
    std::int64_t duration = 0; CHECK(adm::ParseBanDuration("3m", duration) && duration == 180);
    CHECK(adm::ParseBanDuration("2h", duration) && duration == 7200); CHECK(adm::ParseBanDuration("30d", duration) && duration == 2592000);
    CHECK(!adm::ParseBanDuration("31d", duration)); CHECK(!adm::ParseBanDuration("0m", duration)); CHECK(!adm::ParseBanDuration("-1h", duration));
    char banMessage[1024];
    adm::WriteBanTimelimit(-1, banMessage, sizeof(banMessage)); CHECK(!std::strcmp(banMessage, "This is a permanent ban.\n"));
    adm::WriteBanTimelimit(1, banMessage, sizeof(banMessage)); CHECK(std::strstr(banMessage, "0 days 0 hours 1 minutes"));
    adm::WriteBanTimelimit(3599, banMessage, sizeof(banMessage)); CHECK(std::strstr(banMessage, "0 days 1 hours 0 minutes"));
    adm::FormatBanMessage(14399, "Reason", "https://example.test/appeal", 4, banMessage, sizeof(banMessage));
    CHECK(std::strstr(banMessage, "Reason")); CHECK(!std::strstr(banMessage, "https://"));
    adm::FormatBanMessage(14400, "Reason", "https://example.test/appeal", 4, banMessage, sizeof(banMessage));
    CHECK(std::strstr(banMessage, "https://example.test/appeal"));
    adm::FormatBanMessage(-1, "Reason", "https://example.test/appeal", 4, banMessage, sizeof(banMessage)); CHECK(std::strstr(banMessage, "https://"));
    char tiny[3]{'x', 'x', 'x'}; adm::WriteBanTimelimit(INT64_MAX, tiny, sizeof(tiny)); CHECK(tiny[2] == 0);
    CHECK(adm::SafeRotationToken("mp_killhouse")); CHECK(!adm::SafeRotationToken("mp_crash;quit")); CHECK(!adm::SafeRotationToken("../mp_crash"));
    adm::Rotation rotation;
    CHECK(adm::ParseRotation("gametype war map mp_crash gametype sd map mp_killhouse map mp_backlot", "dm", rotation));
    CHECK(rotation.count == 3 && !std::strcmp(rotation.maps[0].gametype, "war") && !std::strcmp(rotation.maps[2].gametype, "sd"));
    char serialized[1024]; CHECK(adm::SerializeRotation(rotation, 1, serialized, sizeof(serialized)));
    CHECK(std::strcmp(serialized, "gametype sd map mp_killhouse gametype sd map mp_backlot") == 0);
    CHECK(adm::ShuffleRotation(rotation));
    unsigned crash = 0, killhouse = 0, backlot = 0;
    for (unsigned i = 0; i != rotation.count; ++i) {
        const auto& entry = rotation.maps[i];
        if (!std::strcmp(entry.map, "mp_crash")) { ++crash; CHECK(!std::strcmp(entry.gametype, "war")); }
        if (!std::strcmp(entry.map, "mp_killhouse")) { ++killhouse; CHECK(!std::strcmp(entry.gametype, "sd")); }
        if (!std::strcmp(entry.map, "mp_backlot")) { ++backlot; CHECK(!std::strcmp(entry.gametype, "sd")); }
    }
    CHECK(crash == 1 && killhouse == 1 && backlot == 1);
    CHECK(!adm::ParseRotation("map mp_crash;quit", "war", rotation)); CHECK(!adm::ParseRotation("gametype war map", "war", rotation));
    adm::IpBanCache ipBans;
    CHECK(ipBans.Add(1, "Reason", 1000, 0)); CHECK(ipBans.Find(1, 15399)); CHECK(!ipBans.Find(1, 15400));
    CHECK(ipBans.Add(1, "Short", 1000, 1060)); CHECK(ipBans.Find(1, 1059)); CHECK(!ipBans.Find(1, 1060));
    CHECK(ipBans.Format(1, 1000, banMessage, sizeof(banMessage)));
    CHECK(!std::strcmp(banMessage, "Short\nThis is a temporary ban and will expire in\n 0 days 0 hours 2 minutes\n\n"));
    CHECK(ipBans.Format(1, 1058, banMessage, sizeof(banMessage)) && std::strstr(banMessage, "1 minutes"));
    CHECK(!ipBans.Format(1, 1059, banMessage, sizeof(banMessage)) && !banMessage[0]);
    CHECK(!ipBans.Format(1, 1060, banMessage, sizeof(banMessage)) && !banMessage[0]);
    CHECK(!ipBans.Format(2, 1000, banMessage, sizeof(banMessage)) && !banMessage[0]);
    char tinyIpMessage[4]{'x', 'x', 'x', 'Z'};
    CHECK(ipBans.Format(1, 1000, tinyIpMessage, 3) && tinyIpMessage[2] == 0 && tinyIpMessage[3] == 'Z');
    CHECK(!ipBans.Add(1, "Reason", 1000, 0, 241)); CHECK(!ipBans.Add(1, "Reason", 1000, 999));
    ipBans.Remove(1); CHECK(!ipBans.Find(1, 1001));
    for (unsigned i = 1; i <= 33; ++i) CHECK(ipBans.Add(i, "Reason", 1000 + i, 0));
    CHECK(!ipBans.Find(1, 1100)); CHECK(ipBans.Find(33, 1100)); // oldest slot evicted, memory remains32 entries
    CHECK(adm::CanUseReservedSlots(10, false, 10)); CHECK(!adm::CanUseReservedSlots(1, false, 10)); CHECK(adm::CanUseReservedSlots(0, true, 10));
    const auto legacyPath = fixture / ("policy-v1-" + std::to_string(GetCurrentProcessId()) + ".bin");
    auto legacy = std::make_unique<LegacyPolicyPayload>(); legacy->admins = store->Data().admins; legacy->bans = store->Data().bans;
    std::copy_n(store->Data().commands.begin(), legacy->commands.size(), legacy->commands.begin());
    LegacyPolicyHeader oldHeader; CHECK(id::Sha256(legacy.get(), sizeof(*legacy), oldHeader.digest));
    { std::ofstream stream(legacyPath, std::ios::binary | std::ios::trunc); CHECK(stream.good());
        stream.write(reinterpret_cast<const char*>(&oldHeader), sizeof(oldHeader)); stream.write(reinterpret_cast<const char*>(legacy.get()), sizeof(*legacy)); CHECK(stream.good()); }
    auto migrated = std::make_unique<adm::Store>(); CHECK(migrated->Load(legacyPath.c_str()));
    CHECK(migrated->Power(master) == 100); CHECK(migrated->CommandPower("kick") == store->CommandPower("kick"));
    CHECK(migrated->RegisterCommand("after_v1_migration", 12) == adm::Result::Ok); CHECK(migrated->Save(legacyPath.c_str()));
    CHECK(migrated->Load(legacyPath.c_str())); CHECK(migrated->CommandPower("after_v1_migration") == 12);
    std::filesystem::remove(legacyPath); std::filesystem::remove(legacyPath.wstring() + L".previous");
    const auto policyPath = fixture / ("policy-" + std::to_string(GetCurrentProcessId()) + ".bin");
    CHECK(store->Save(policyPath.c_str())); CHECK(store->Save(policyPath.c_str()));
    auto loaded = std::make_unique<adm::Store>(); CHECK(loaded->Load(policyPath.c_str())); CHECK(loaded->Power(master) == 100);
    CHECK(loaded->Authorize("Master", "replacement-123")); CHECK(loaded->CommandPower("map") == 100);
    { std::fstream file(policyPath, std::ios::binary | std::ios::in | std::ios::out); CHECK(file);
      file.seekg(200); char byte = 0; file.get(byte); CHECK(file); file.seekp(200); file.put(char(static_cast<unsigned char>(byte) ^ 1)); }
    CHECK(!loaded->Load(policyPath.c_str())); CHECK(loaded->Power(master) == 100); // fail closed, preserve active model
    std::filesystem::remove(policyPath); std::filesystem::remove(policyPath.wstring() + L".previous");
    adm::RconLimiter limiter;
    CHECK(limiter.Validate(1, 0, "password-123", "password-123") == adm::RconResult::Accepted);
    CHECK(limiter.Validate(2, 0, "wrong", "password-123") == adm::RconResult::BadPassword);
    CHECK(limiter.Validate(3, 0, "wrong", "password-123") == adm::RconResult::Limited);
    CHECK(limiter.Validate(4, 0, "password-123", "password-123") == adm::RconResult::Accepted);
    CHECK(limiter.Validate(2, 100, "wrong", "password-123") == adm::RconResult::BadPassword);
    for (int i = 0; i < 8; ++i) CHECK(limiter.Validate(5, 200, "password-123", "password-123") == adm::RconResult::Accepted);
    CHECK(limiter.Validate(5, 200, "password-123", "password-123") == adm::RconResult::Limited);
    CHECK(limiter.Validate(5, 300, "password-123", "password-123") == adm::RconResult::Accepted);
    CHECK(limiter.Validate(6, 400, "short", "short") == adm::RconResult::Disabled);
    CHECK(adm::ConstantTimeEqual("abc", "abc")); CHECK(!adm::ConstantTimeEqual("abc", "abcd"));
    auto capacity = std::make_unique<adm::Store>();
    for (unsigned i = 0; i != adm::MaxAdmins; ++i) CHECK(capacity->SetAdmin(console, fingerprint(10000 + i), "Admin" + std::to_string(i), 10) == adm::Result::Ok);
    CHECK(capacity->SetAdmin(console, fingerprint(20000), "Extra", 10) == adm::Result::Full);
    for (unsigned i = 0; i != adm::MaxBans; ++i) CHECK(capacity->AddBan(console, {fingerprint(30000 + i)}, "Player", "Reason", 1000, 0, false) == adm::Result::Ok);
    CHECK(capacity->AddBan(console, {fingerprint(50000)}, "Player", "Reason", 1000, 0, false) == adm::Result::Full);
    for (unsigned i = 0; i != adm::MaxCommandRules; ++i) CHECK(capacity->RegisterCommand("command" + std::to_string(i), 1) == adm::Result::Ok);
    CHECK(capacity->RegisterCommand("extra_command", 1) == adm::Result::Full);
    std::puts("identity/admin tests passed");
}
