#include <universal/q_shared.h>
#include "khz_profile.h"
#include "khz_profile_audit.h"
#include "khz_identity.h"
#include "khz_client_owner.h"
#include <qcommon/threads.h>
#include <unlock_audit_policy.hpp>
#include "khz_paths.h"
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/com_playerprofile.h>
#include <universal/com_files.h>
#include <win32/win_storage.h>
#include <database/database.h>
#include <client_mp/client_mp.h>
#include <Windows.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr unsigned StatCount = 3498;
constexpr unsigned StatsBytes = 8192;
constexpr unsigned UnlockMagic = 0x3155484b; // KHU1, little endian Win32 format.
constexpr const char* UnlockRecordFilename = "khz_unlocks.bin";
struct UnlockPlan {
    std::array<std::uint32_t, StatCount> masks{};
    unsigned minimumXp = 0;
    unsigned weapons = 0, attachments = 0, camos = 0, perks = 0, features = 0;
};
struct UnlockRecord {
    std::uint32_t magic = UnlockMagic, version = 1, count = StatCount;
    std::uint32_t minimumXp = 0, checksum = 0;
    std::array<std::uint32_t, StatCount> masks{};
};
static_assert(sizeof(UnlockRecord) == 20 + StatCount * sizeof(std::uint32_t));
UnlockRecord entitlements;
bool entitlementsLoaded = false;
std::string entitlementProfile, entitlementHome;
unsigned fileSerial = 0;
cmd_function_s fovCommand{}, unlockCommand{}, selfTestCommand{}, restoreCommand{};
bool (*connectedUnlockHandler)() = nullptr;

bool ParseUnsigned(const char* text, unsigned* value)
{
    if (!text || !*text) return false;
    unsigned long long parsed = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        parsed = parsed * 10 + (*p - '0');
        if (parsed > 0x7fffffffu) return false;
    }
    *value = static_cast<unsigned>(parsed);
    return true;
}

std::uint32_t HashBytes(const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

unsigned StatOffset(unsigned index) { return index < 2000 ? index + 4 : index * 4 - 5996; }
std::uint32_t ReadStat(const unsigned char* data, unsigned index)
{
    if (index < 2000) return data[StatOffset(index)];
    std::uint32_t value;
    memcpy(&value, data + StatOffset(index), sizeof(value));
    return value;
}
void WriteStat(unsigned char* data, unsigned index, std::uint32_t value)
{
    if (index < 2000) data[StatOffset(index)] = static_cast<unsigned char>(value);
    else memcpy(data + StatOffset(index), &value, sizeof(value));
}
std::uint32_t PreservedStatsHash(const unsigned char* data, const UnlockPlan& plan)
{
    std::uint32_t hash = 2166136261u;
    for (unsigned index = 0; index < StatCount; ++index) {
        if (index == 2301 || plan.masks[index]) continue;
        const std::uint32_t value = ReadStat(data, index);
        for (unsigned shift = 0; shift < 32; shift += 8) hash = (hash ^ ((value >> shift) & 255u)) * 16777619u;
    }
    return hash;
}
bool OwnershipIndex(unsigned index)
{
    // Stock ownership bytes and packed weapon unlock flags. Challenge progress,
    // combat counters, custom-class choices, prestige and challenge tiers stay out.
    return index < 200 || index == 200 || (index >= 250 && index < 270)
        || (index >= 3000 && index < 3150);
}

const StringTable* Table(const char* name, int columns)
{
    const auto* table = DB_FindXAssetHeader(ASSET_TYPE_STRINGTABLE, name).stringTable;
    if (!table || DB_IsXAssetDefault(ASSET_TYPE_STRINGTABLE, name) || !table->values
        || table->columnCount < columns || table->rowCount < 2 || table->rowCount > 1024)
        return nullptr;
    return table;
}
const char* Cell(const StringTable* table, int row, int column)
{
    return StringTable_GetColumnValueForRow(table, row, column);
}
bool Same(const char* a, const char* b) { return !I_stricmp(a, b); }
bool PrimaryGroup(const char* group)
{
    return Same(group, "weapon_smg") || Same(group, "weapon_assault")
        || Same(group, "weapon_sniper") || Same(group, "weapon_shotgun") || Same(group, "weapon_lmg");
}
bool HasToken(const char* words, const char* token)
{
    const size_t length = strlen(token);
    while (*words) {
        while (*words == ' ' || *words == '\t') ++words;
        const char* end = words;
        while (*end && *end != ' ' && *end != '\t') ++end;
        if (static_cast<size_t>(end - words) == length && !I_strnicmp(words, token, length)) return true;
        words = end;
    }
    return false;
}

bool MakePlan(UnlockPlan* plan, std::string* error)
{
    *plan = UnlockPlan{};
    const auto* items = Table("mp/statstable.csv", 11);
    const auto* addons = Table("mp/attachmenttable.csv", 12);
    const auto* ranks = Table("mp/ranktable.csv", 15);
    if (!items || !addons || !ranks) {
        *error = "The stock multiplayer unlock tables are not loaded."; return false;
    }
    unsigned maximum = 0, display = 0;
    if (!ParseUnsigned(StringTable_Lookup(ranks, 0, "maxrank", 1), &maximum) || maximum != 54
        || !ParseUnsigned(StringTable_Lookup(ranks, 0, "54", 14), &display) || display != 55
        || !ParseUnsigned(StringTable_Lookup(ranks, 0, "54", 2), &plan->minimumXp) || !plan->minimumXp) {
        *error = "The loaded rank table does not describe stock level 55."; return false;
    }
    struct Addon { std::string reference; unsigned mask = 0; bool camo = false; };
    std::vector<Addon> available;
    for (int row = 0; row < addons->rowCount; ++row) {
        const char* group = Cell(addons, row, 2);
        const bool camo = Same(group, "camo");
        unsigned id = 0, mask = 0;
        if (!camo && I_strnicmp(group, "attachment_", 11)) continue;
        if (!ParseUnsigned(Cell(addons, row, camo ? 11 : 9), &id) || id > 7
            || !ParseUnsigned(Cell(addons, row, 10), &mask) || !mask || mask > 0xffffu
            || (mask & (mask - 1)) || !*Cell(addons, row, 4)) {
            *error = "An attachment/camo row has an invalid stock ownership mask."; return false;
        }
        available.push_back({ Cell(addons, row, 4), mask, camo });
        if (camo) ++plan->camos; else ++plan->attachments;
    }
    bool m16 = false;
    for (int row = 0; row < items->rowCount; ++row) {
        const char* reference = Cell(items, row, 4);
        const char* group = Cell(items, row, 2);
        if (!*reference) continue;
        const bool primary = PrimaryGroup(group);
        const bool weapon = primary || Same(group, "weapon_pistol") || Same(group, "weapon_projectile")
            || Same(group, "weapon_grenade") || Same(group, "weapon_explosive");
        const bool perk = Same(group, "specialty") || Same(group, "inventory") || Same(group, "grenade")
            || Same(group, "specialgrenade") || Same(group, "null_specialty");
        const bool feature = Same(group, "feature");
        if (!weapon && !perk && !feature) continue;
        unsigned index = 0;
        if (!ParseUnsigned(Cell(items, row, 1), &index) || index >= StatCount || !OwnershipIndex(index)
            || (weapon && (index < 3000 || index >= 3150)) || (perk && index >= 200)
            || (feature && (index < 250 || index >= 270))) {
            *error = "An item row points outside the stock ownership stat ranges."; return false;
        }
        unsigned mask = 1;
        if (weapon) {
            ++plan->weapons;
            for (const auto& addon : available)
                if ((addon.camo && primary) || (!addon.camo && HasToken(Cell(items, row, 8), addon.reference.c_str())))
                    mask |= addon.mask;
            if (Same(reference, "m16") && index == 3025) m16 = true;
        } else if (perk) ++plan->perks;
        else {
            ++plan->features;
            if (Same(reference, "feature_cac")) plan->masks[200] |= 1;
        }
        plan->masks[index] |= mask;
    }
    if (!m16 || plan->weapons < 25 || plan->perks < 15 || plan->features < 5
        || plan->attachments < 5 || plan->camos < 6) {
        *error = "The stock unlock tables are incomplete; no profile changes were made."; return false;
    }
    return true;
}

void ApplyPlan(const UnlockPlan& plan, unsigned char* data)
{
    for (unsigned i = 0; i < StatCount; ++i)
        if (plan.masks[i]) WriteStat(data, i, ReadStat(data, i) | plan.masks[i]);
    if (ReadStat(data, 2301) < plan.minimumXp) WriteStat(data, 2301, plan.minimumXp);
}

bool ValidUnlockRecord(UnlockRecord record, const unsigned char* stats)
{
    const auto checksum = record.checksum;
    record.checksum = 0;
    if (!stats || record.magic != UnlockMagic || record.version != 1 || record.count != StatCount
        || !record.minimumXp || HashBytes(&record, sizeof(record)) != checksum) return false;
    UnlockPlan stock;
    std::string error;
    if (!MakePlan(&stock, &error) || record.minimumXp != stock.minimumXp || record.masks != stock.masks)
        return false;
    for (unsigned index = 0; index < StatCount; ++index)
        if (record.masks[index] && (ReadStat(stats, index) & record.masks[index]) != record.masks[index])
            return false;
    return ReadStat(stats, 2301) >= record.minimumXp;
}

bool PathWithin(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c)
        if (c == child.end() || CompareStringOrdinal(c->c_str(), -1, p->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    return c != child.end();
}
bool ProfilePath(std::filesystem::path* path, std::string* error)
{
    if (KHZ_IsBenchmarkHome() && !Dvar_GetBool("khz_test_controls")) {
        *error = "Unlock transactions require an interactive profile or explicit isolated test controls."; return false;
    }
    if (!Com_HasPlayerProfile() || !com_playerProfile || !*com_playerProfile->current.string || !fs_homepath
        || !fs_gameDirVar || *fs_gameDirVar->current.string) {
        *error = "Select a normal stock multiplayer profile first."; return false;
    }
    const std::string profile = com_playerProfile->current.string;
    if (profile == "." || profile == ".." || profile.find_first_of("/\\:") != std::string::npos) {
        *error = "The active profile path is invalid."; return false;
    }
    std::error_code ec;
    const auto project = std::filesystem::weakly_canonical(KHZ_ProjectRoot(), ec);
    if (ec) { *error = "The project directory is unavailable."; return false; }
    const auto expectedHome = std::filesystem::weakly_canonical(KHZ_HomeRoot(), ec);
    if (ec || !PathWithin(expectedHome, project)) { *error = "The profile home is outside this project."; return false; }
    const auto home = std::filesystem::weakly_canonical(fs_homepath->current.string, ec);
    if (ec || CompareStringOrdinal(home.c_str(), -1, expectedHome.c_str(), -1, TRUE) != CSTR_EQUAL) {
        *error = "The active filesystem home does not match the project profile home."; return false;
    }
    auto selected = home / "players" / "profiles" / profile;
    for (const auto& directory : { home, home / "players", home / "players" / "profiles", selected })
        if (GetFileAttributesW(directory.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) {
            *error = "Profile backups require ordinary project directories."; return false;
        }
    selected = std::filesystem::canonical(selected, ec);
    if (ec || !PathWithin(selected, home) || !std::filesystem::is_directory(selected, ec) || ec) {
        *error = "The active profile directory is unavailable."; return false;
    }
    *path = selected;
    return true;
}

bool AtomicWrite(const std::filesystem::path& path, const void* data, size_t length)
{
    const auto temporary = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++fileSerial);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = length <= MAXDWORD && WriteFile(file, data, static_cast<DWORD>(length), &written, nullptr)
        && written == length && FlushFileBuffers(file);
    ok = CloseHandle(file) && ok;
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}
bool ReadBytes(const std::filesystem::path& path, std::vector<unsigned char>* bytes, size_t limit)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit) return false;
    bytes->resize(static_cast<size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.read(reinterpret_cast<char*>(bytes->data()), bytes->size());
    return stream.good() && static_cast<size_t>(stream.gcount()) == bytes->size();
}

bool BackupProfile(const std::filesystem::path& profile, const playerStatNetworkData& live,
    std::filesystem::path* backup, std::string* error)
{
    std::error_code ec;
    const auto backups = profile / "khz_backups";
    if (std::filesystem::exists(backups, ec) && (ec || (GetFileAttributesW(backups.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))) {
        *error = "The profile backup directory is invalid."; return false;
    }
    std::filesystem::create_directories(backups, ec);
    if (ec) { *error = "Cannot create the profile backup directory."; return false; }
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    char name[96];
    snprintf(name, sizeof(name), "%04u%02u%02uT%02u%02u%02u.%03uZ-%lu-%u", utc.wYear, utc.wMonth, utc.wDay,
        utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds, GetCurrentProcessId(), ++fileSerial);
    *backup = backups / name;
    if (!std::filesystem::create_directory(*backup, ec) || ec) { *error = "Cannot create a unique profile backup."; return false; }
    unsigned files = 0;
    std::uintmax_t totalBytes = 0;
    std::filesystem::recursive_directory_iterator it(profile, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto relative = it->path().lexically_relative(profile);
        if (relative.empty() || relative.is_absolute()) { ec = std::make_error_code(std::errc::invalid_argument); break; }
        if (Same(relative.begin()->string().c_str(), "khz_backups")) { it.disable_recursion_pending(); continue; }
        if (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) {
            ec = std::make_error_code(std::errc::too_many_symbolic_link_levels); break;
        }
        const auto destination = *backup / relative;
        if (it->is_directory(ec)) std::filesystem::create_directory(destination, ec);
        else if (it->is_regular_file(ec)) {
            totalBytes += it->file_size(ec);
            if (ec || ++files > 1024 || totalBytes > 64u * 1024u * 1024u) {
                ec = std::make_error_code(std::errc::file_too_large); break;
            }
            std::filesystem::copy_file(it->path(), destination, std::filesystem::copy_options::none, ec);
            if (!ec) {
                std::vector<unsigned char> before, after;
                if (!ReadBytes(it->path(), &before, 64u * 1024u * 1024u) || !ReadBytes(destination, &after, before.size()) || before != after)
                    ec = std::make_error_code(std::errc::io_error);
            }
        } else if (!ec) ec = std::make_error_code(std::errc::invalid_argument);
    }
    if (ec || !AtomicWrite(*backup / "memory_stats.bin", live.playerStats, sizeof(live.playerStats))) {
        *error = "The profile backup could not be completed and verified; no unlocks were applied."; return false;
    }
    std::ostringstream manifest;
    manifest << "cod4x1khz profile backup v1\nprofile=" << com_playerProfile->current.string
        << "\nstats_fetched=" << live.statsFetched << "\nstats_write_needed=" << live.statWriteNeeded
        << "\nmemory_stats_fnv1a=" << HashBytes(live.playerStats, sizeof(live.playerStats))
        << "\ncopied_files=" << files << "\ncopied_bytes=" << totalBytes << "\n";
    const auto text = manifest.str();
    if (!AtomicWrite(*backup / "backup.txt", text.data(), text.size())) {
        *error = "The profile backup manifest could not be written; no unlocks were applied."; return false;
    }
    return true;
}

void FovScaleCommand()
{
    const auto* dvar = Dvar_FindVar("cg_fovScale");
    if (Cmd_Argc() == 1) { Com_Printf(CON_CHANNEL_CLIENT, "fovscale = %.3f (0.2 to 2.0)\n", dvar->current.value); return; }
    char* end = nullptr;
    errno = 0;
    const char* text = Cmd_Argv(1);
    const float scale = strtof(text, &end);
    if (Cmd_Argc() != 2 || !*text || !end || *end || errno || !std::isfinite(scale) || scale < 0.2f || scale > 2.0f) {
        Com_Printf(CON_CHANNEL_CLIENT, "Usage: fovscale <0.2 to 2.0>\n"); return;
    }
    Dvar_SetFloatByName("cg_fovScale", scale);
    Com_Printf(CON_CHANNEL_CLIENT, "fovscale set to %.3f; archived with this profile's settings.\n", scale);
}

#include "khz_profile_audit.inl"

bool CommitUnlocks()
{
    std::filesystem::path profile, backup;
    std::string error;
    UnlockPlan plan;
    if (!ProfilePath(&profile, &error) || !MakePlan(&plan, &error)) {
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: %s\n", error.c_str()); return false;
    }
    if (!LiveStorage_DoWeHaveStats()) {
        // Loading a fresh profile can initialize stock stats/class-name dvars.
        // Preserve its files first, before even that normal initialization path.
        const playerStatNetworkData unloaded{};
        std::filesystem::path initialBackup;
        if (!BackupProfile(profile, unloaded, &initialBackup, &error)) {
            Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: %s\n", error.c_str()); return false;
        }
        LiveStorage_ReadStats();
    }
    if (!LiveStorage_DoWeHaveStats()) { Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Player stats could not be loaded.\n"); return false; }
    auto* live = LiveStorage_GetStatBuffer();
    const playerStatNetworkData original = *live;
    std::array<unsigned char, StatsBytes> candidate;
    memcpy(candidate.data(), original.playerStats, candidate.size());
    ApplyPlan(plan, candidate.data());
    for (unsigned index = 0; index < StatCount; ++index) {
        if (index != 2301 && !plan.masks[index] && ReadStat(candidate.data(), index) != ReadStat(original.playerStats, index)) {
            Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Combat/challenge preservation validation failed; profile is unchanged.\n");
            return false;
        }
    }
    LiveStorage_WriteChecksumToBuffer(candidate.data(), candidate.size());
    StatsFile encoded;
    if (!LiveStorage_KHZEncodeStats(candidate.data(), &encoded)) {
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Candidate stats did not pass encrypted-file validation.\n"); return false;
    }
    if (!BackupProfile(profile, original, &backup, &error)) {
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: %s\n", error.c_str()); return false;
    }
    UnlockRecord record;
    record.minimumXp = plan.minimumXp;
    record.masks = plan.masks;
    record.checksum = HashBytes(&record, sizeof(record));
    const auto sidecar = profile / UnlockRecordFilename;
    std::error_code ec;
    const bool existed = std::filesystem::exists(sidecar, ec);
    std::vector<unsigned char> prior;
    if (ec || (existed && !ReadBytes(sidecar, &prior, 32768))) {
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Cannot read the previous entitlement record; profile is unchanged.\n"); return false;
    }
    if (!AtomicWrite(sidecar, &record, sizeof(record))) {
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Cannot commit the entitlement record; profile is unchanged.\n"); return false;
    }
    if (!AtomicWrite(profile / "mpdata", &encoded, sizeof(encoded))) {
        const bool restored = existed ? AtomicWrite(sidecar, prior.data(), prior.size()) : DeleteFileW(sidecar.c_str()) != FALSE;
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Cannot commit mpdata. Live stats are unchanged. Entitlement rollback %s. Backup: %s\n",
            restored ? "succeeded" : "failed", backup.string().c_str());
        return false;
    }
    memcpy(live->playerStats, candidate.data(), candidate.size());
    live->statWriteNeeded = false;
    KHZ_ProfileReloadEntitlements();
    CaptureSuccessfulUnlockAudit(original,candidate,plan,record,encoded,profile,backup);
    Com_Printf(CON_CHANNEL_CLIENT,
        "KHZ_UNLOCK_TRANSACTION {\"committed\":true,\"preserved_before\":%u,\"preserved_after\":%u,\"minimum_xp\":%u,\"backup_completed\":true}\n",
        PreservedStatsHash(original.playerStats, plan), PreservedStatsHash(candidate.data(), plan), plan.minimumXp);
    Com_Printf(CON_CHANNEL_CLIENT, "Unlocked level 55, %u stock weapons, their supported attachments/camos, and stock perks/features.\n",
        plan.weapons);
    Com_Printf(CON_CHANNEL_CLIENT, "Combat stats, challenge progress, prestige, and custom-class selections were preserved. Backup: %s\n",
        backup.string().c_str());
    return true;
}

const char* ProfileStatusJSON()
{
    static char result[4096];
    UnlockPlan plan;
    std::string error;
    const bool loaded = LiveStorage_DoWeHaveStats();
    const bool tables = MakePlan(&plan, &error);
    const auto* data = loaded ? LiveStorage_GetStatBuffer()->playerStats : nullptr;
    int used = std::snprintf(result, sizeof(result),
        "{\"loaded\":%s,\"tables\":%s,\"xp\":%u,\"preserved_stats_hash\":%u,\"all_unlocks\":%s,\"weapons\":%u,\"attachments\":%u,\"camos\":%u,\"classes\":[",
        loaded ? "true" : "false", tables ? "true" : "false", data ? ReadStat(data, 2301) : 0,
        data && tables ? PreservedStatsHash(data, plan) : 0, KHZ_ProfileHasAllUnlocks() ? "true" : "false",
        plan.weapons, plan.attachments, plan.camos);
    if (tables && data) {
        const auto* items = Table("mp/statstable.csv", 11);
        const auto* addons = Table("mp/attachmenttable.csv", 12);
        const auto reference = [](const StringTable* table, unsigned column, unsigned id) {
            char number[16]; std::snprintf(number, sizeof(number), "%u", id);
            const char* value = StringTable_Lookup(table, column, number, 4);
            // Names are diagnostic identifiers, never executable menu text.
            if (!value || std::strlen(value) > 63) return "";
            for (const char* p = value; *p; ++p)
                if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return "";
            return value;
        };
        for (unsigned slot = 0; slot < 5; ++slot) {
            const unsigned base = 201 + slot * 10;
            const unsigned primary = ReadStat(data, base), attachment = ReadStat(data, base + 1), secondary = ReadStat(data, base + 2);
            const unsigned camo = ReadStat(data, base + 8), secondaryAttachment = ReadStat(data, base + 3), grenade = ReadStat(data, base + 7);
            used += std::snprintf(result + used, sizeof(result) - used,
                "%s{\"slot\":%u,\"primary_id\":%u,\"primary\":\"%s\",\"attachment_id\":%u,\"attachment\":\"%s\",\"secondary_id\":%u,\"secondary\":\"%s\",\"secondary_attachment_id\":%u,\"secondary_attachment\":\"%s\",\"special_grenade_id\":%u,\"special_grenade\":\"%s\",\"camo_id\":%u,\"camo\":\"%s\",\"perk_ids\":[%u,%u,%u]}",
                slot ? "," : "", slot + 1, primary, reference(items, 0, primary), attachment, reference(addons, 9, attachment),
                secondary, reference(items, 0, secondary), secondaryAttachment, reference(addons, 9, secondaryAttachment), grenade, reference(items, 0, grenade), camo, reference(addons, 11, camo),
                ReadStat(data, base + 4), ReadStat(data, base + 5), ReadStat(data, base + 6));
        }
    }
    std::snprintf(result + used, sizeof(result) - used, "]}");
    return result;
}

void UnlockAllCommand()
{
    if (Cmd_Argc() != 1) { Com_Printf(CON_CHANNEL_CLIENT, "Usage: unlockall\n"); return; }
    if (CL_GetLocalClientConnectionState(0) != CA_DISCONNECTED) {
        if (connectedUnlockHandler) connectedUnlockHandler();
        else Com_Printf(CON_CHANNEL_CLIENT, "Return to the main menu before using unlockall; your next connection will send the updated profile.\n");
        return;
    }
    KHZ_ProfileCommitAllUnlocks();
}
void RestoreProfileCommand()
{
    if (Cmd_Argc() != 2 || CL_GetLocalClientConnectionState(0) != CA_DISCONNECTED) {
        Com_Printf(CON_CHANNEL_CLIENT, "Usage: restoreunlocks <backup-directory-name> from the main menu\n"); return;
    }
    const char* name = Cmd_Argv(1);
    if (!name || !*name || std::strlen(name) > 95 || std::strstr(name, "..") || std::strpbrk(name, "/\\:")) {
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Invalid backup name.\n"); return;
    }
    std::filesystem::path profile, safetyBackup;
    std::string error;
    if (!ProfilePath(&profile, &error) || !LiveStorage_DoWeHaveStats()) {
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Select a loaded stock profile. %s\n", error.c_str()); return;
    }
    std::error_code ec;
    const auto backup = profile / "khz_backups" / name;
    for (const auto& path : {profile / "khz_backups", backup, backup / "memory_stats.bin", backup / "backup.txt"}) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Backup files are unavailable or invalid.\n"); return;
        }
    }
    std::vector<unsigned char> memory, manifest;
    if (!ReadBytes(backup / "memory_stats.bin", &memory, StatsBytes) || memory.size() != StatsBytes ||
        !ReadBytes(backup / "backup.txt", &manifest, 4096)) {
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Backup is incomplete.\n"); return;
    }
    const std::string text(manifest.begin(), manifest.end());
    const std::string selected = std::string("\nprofile=") + com_playerProfile->current.string + "\n";
    const std::string checksum = std::string("\nmemory_stats_fnv1a=") + std::to_string(HashBytes(memory.data(), memory.size())) + "\n";
    if (text.rfind("cod4x1khz profile backup v1\n", 0) || text.find(selected) == std::string::npos || text.find(checksum) == std::string::npos) {
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Backup identity or checksum differs.\n"); return;
    }
    StatsFile encoded;
    LiveStorage_WriteChecksumToBuffer(memory.data(), memory.size());
    if (!LiveStorage_KHZEncodeStats(memory.data(), &encoded)) {
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Backup stats failed encoded-file validation.\n"); return;
    }
    const auto sidecar = profile / UnlockRecordFilename;
    const auto oldSidecar = backup / UnlockRecordFilename;
    const bool hasPrior = std::filesystem::exists(oldSidecar, ec);
    std::vector<unsigned char> prior, current;
    if (ec || (hasPrior && ((GetFileAttributesW(oldSidecar.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) || !ReadBytes(oldSidecar, &prior, sizeof(UnlockRecord)) || prior.size() != sizeof(UnlockRecord)))) {
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Backup entitlement record is invalid.\n"); return;
    }
    if (hasPrior) {
        UnlockRecord record;
        std::memcpy(&record, prior.data(), sizeof(record));
        if (!ValidUnlockRecord(record, memory.data())) {
            Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Backup entitlement checksum, stock masks or stats differ.\n"); return;
        }
    }
    const bool hasCurrent = std::filesystem::exists(sidecar, ec);
    if (ec || (hasCurrent && !ReadBytes(sidecar, &current, sizeof(UnlockRecord)))) return;
    auto* live = LiveStorage_GetStatBuffer();
    if (!BackupProfile(profile, *live, &safetyBackup, &error)) {
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: %s\n", error.c_str()); return;
    }
    bool entitlementSaved = hasPrior ? AtomicWrite(sidecar, prior.data(), prior.size()) :
        (!hasCurrent || DeleteFileW(sidecar.c_str()) != FALSE);
    if (!entitlementSaved || !AtomicWrite(profile / "mpdata", &encoded, sizeof(encoded))) {
        const bool rolledBack = hasCurrent ? AtomicWrite(sidecar, current.data(), current.size()) :
            (!std::filesystem::exists(sidecar) || DeleteFileW(sidecar.c_str()) != FALSE);
        Com_PrintError(CON_CHANNEL_CLIENT, "restoreunlocks: Commit failed; live stats unchanged; entitlement rollback %s.\n", rolledBack ? "succeeded" : "failed"); return;
    }
    std::memcpy(live->playerStats, memory.data(), memory.size());
    live->statWriteNeeded = false;
    KHZ_ProfileReloadEntitlements();
    Com_Printf(CON_CHANNEL_CLIENT, "KHZ_PROFILE_RESTORE {\"restored\":true,\"stats_hash\":%u,\"safety_backup\":true}\n", HashBytes(live->playerStats, sizeof(live->playerStats)));
}
}

#include "khz_profile_audit_public.inl"

bool KHZ_ProfileCommitAllUnlocks() { return CommitUnlocks(); }
const char* KHZ_ProfileStatusJSON() { return ProfileStatusJSON(); }
void KHZ_ProfileSetConnectedUnlockHandler(bool (*handler)()) { connectedUnlockHandler = handler; }

void KHZ_ProfileReset()
{
    entitlementsLoaded = false;
    entitlementProfile.clear();
    entitlementHome.clear();
    entitlements = UnlockRecord{};
}

void KHZ_ProfileReloadEntitlements()
{
    KHZ_ProfileReset();
    if (!LiveStorage_DoWeHaveStats()) return;
    std::filesystem::path profile;
    std::string error;
    if (!ProfilePath(&profile, &error)) return;
    std::vector<unsigned char> bytes;
    if (!ReadBytes(profile / UnlockRecordFilename, &bytes, sizeof(UnlockRecord)) || bytes.size() != sizeof(UnlockRecord)) return;
    UnlockRecord record;
    memcpy(&record, bytes.data(), sizeof(record));
    if (!ValidUnlockRecord(record, LiveStorage_GetStatBuffer()->playerStats)) return;
    entitlements = record;
    entitlementsLoaded = true;
    entitlementProfile = com_playerProfile->current.string;
    entitlementHome = fs_homepath->current.string;
}

bool KHZ_ProfileHasAllUnlocks()
{
    return entitlementsLoaded && LiveStorage_DoWeHaveStats() &&
        (!KHZ_IsBenchmarkHome() || Dvar_GetBool("khz_test_controls"))
        && com_playerProfile && fs_homepath && fs_gameDirVar && !*fs_gameDirVar->current.string
        && entitlementProfile == com_playerProfile->current.string && entitlementHome == fs_homepath->current.string;
}
std::uint32_t KHZ_ProfileUnlockMask(int statIndex)
{
    return statIndex >= 0 && statIndex < StatCount && KHZ_ProfileHasAllUnlocks() ? entitlements.masks[statIndex] : 0;
}

bool KHZ_ProfileSelfTest()
{
    UnlockPlan plan;
    std::string error;
    if (!MakePlan(&plan, &error)) { Com_PrintError(CON_CHANNEL_CLIENT, "KHZ_PROFILE_TEST {\"passed\":false,\"tables_loaded\":false}\n"); return false; }
    std::array<unsigned char, StatsBytes> original{}, candidate{};
    for (unsigned i = 0; i < StatsBytes; ++i) original[i] = static_cast<unsigned char>((i * 73 + 19) & 255);
    WriteStat(original.data(), 2301, 0);
    candidate = original;
    ApplyPlan(plan, candidate.data());
    std::array<bool, StatsBytes> allowed{};
    unsigned changes = 0;
    for (unsigned i = 0; i < StatCount; ++i) {
        if (plan.masks[i] || i == 2301) {
            const unsigned size = i < 2000 ? 1 : 4;
            for (unsigned byte = 0; byte < size; ++byte) allowed[StatOffset(i) + byte] = true;
        }
        if (ReadStat(original.data(), i) != ReadStat(candidate.data(), i)) ++changes;
    }
    bool preserved = true, granted = ReadStat(candidate.data(), 2301) == plan.minimumXp;
    for (unsigned i = 0; i < StatsBytes; ++i) if (!allowed[i] && candidate[i] != original[i]) preserved = false;
    for (unsigned i = 0; i < StatCount; ++i)
        if (plan.masks[i] && ((ReadStat(candidate.data(), i) & plan.masks[i]) != plan.masks[i]
            || (ReadStat(candidate.data(), i) & ReadStat(original.data(), i)) != ReadStat(original.data(), i))) granted = false;
    const auto once = candidate;
    ApplyPlan(plan, candidate.data());
    const bool idempotent = candidate == once;
    WriteStat(candidate.data(), 2301, plan.minimumXp + 1000);
    ApplyPlan(plan, candidate.data());
    const bool xpPreserved = ReadStat(candidate.data(), 2301) == plan.minimumXp + 1000;
    bool fov = KHZ_ScaledFov(120, 2, 10) == 160 && KHZ_ScaledFov(65, 1, 10) == 65;
    for (const float scale : { 0.2f, 1.0f, 1.5f, 2.0f }) {
        const float hip = KHZ_ScaledFov(65, scale, 10);
        const double denominator = tan(hip * 0.017453292519943295 * 0.5);
        float previous = hip;
        for (unsigned step = 0; step <= 100; ++step) {
            const float view = KHZ_ScaledFov(65 - 25 * (step / 100.0f), scale, 10);
            const double ratio = tan(view * 0.017453292519943295 * 0.5) / denominator;
            if (!std::isfinite(ratio) || ratio <= 0 || ratio > 1.000001 || view > previous) fov = false;
            previous = view;
        }
    }
    const bool passed = preserved && granted && idempotent && xpPreserved && fov;
    Com_Printf(CON_CHANNEL_CLIENT, "KHZ_PROFILE_TEST {\"passed\":%s,\"tables_loaded\":true,\"preserved\":%s,\"granted\":%s,\"idempotent\":%s,\"higher_xp_preserved\":%s,\"fov_ads\":%s,\"changed_stats\":%u,\"weapons\":%u,\"perks\":%u,\"attachments\":%u,\"camos\":%u,\"minimum_xp\":%u,\"file_writes\":0}\n",
        passed ? "true" : "false", preserved ? "true" : "false", granted ? "true" : "false", idempotent ? "true" : "false",
        xpPreserved ? "true" : "false", fov ? "true" : "false", changes, plan.weapons, plan.perks, plan.attachments, plan.camos, plan.minimumXp);
    return passed;
}

void KHZ_InitProfileFeatures()
{
#ifndef KISAK_DEDICATED
    Dvar_RegisterFloat("cg_fovScale", 1.0f, 0.2f, 2.0f, DVAR_ARCHIVE, "Scale applied to the field of view");
    Cmd_AddCommandInternal("fovscale", FovScaleCommand, &fovCommand);
    Cmd_AddCommandInternal("unlockall", UnlockAllCommand, &unlockCommand);
    Cmd_AddCommandInternal("restoreunlocks", RestoreProfileCommand, &restoreCommand);
    Cmd_AddCommandInternal("khz_profile_selftest", [] { KHZ_ProfileSelfTest(); }, &selfTestCommand);
#endif
}
