#pragma once
#include "khz_content.h"
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace khz::update {
inline constexpr unsigned MaxManifestBytes = 8192;
// Packages and recovery journals are scoped to the compiled application ABI.
#ifdef _WIN64
#define KHZ_UPDATE_DIRECTORY "updates/x64"
inline constexpr char Platform[] = "windows-x64";
inline constexpr unsigned BinaryMachine = 0x8664, OptionalMagic = 0x20b, OptionalBytes = 240;
inline constexpr char HelperPath[] = "runtime/x64/cod4x1khz_update.exe";
inline constexpr std::string_view ClientMirrors = "client_mirrors=runtime/x64/cod4x1khz_bench.exe\n";
inline constexpr std::array<const char*, 2> PackagePaths{
    "runtime/x64/cod4x1khz.exe", "runtime/x64/cod4x1khz_server.exe"};
inline constexpr std::array<const char*, 3> InstalledPaths{
    PackagePaths[0], PackagePaths[1], "runtime/x64/cod4x1khz_bench.exe"};
inline constexpr std::array<unsigned, 3> InstalledSourceIndexes{0, 1, 0};
#else
#define KHZ_UPDATE_DIRECTORY "updates"
inline constexpr char Platform[] = "windows-x86";
inline constexpr unsigned BinaryMachine = 0x14c, OptionalMagic = 0x10b, OptionalBytes = 224;
inline constexpr char HelperPath[] = "runtime/cod4x1khz_update.exe";
inline constexpr std::string_view ClientMirrors = "client_mirrors=runtime/cod4x1khz.exe,runtime/cod4x1khz_bench.exe\n";
inline constexpr std::array<const char*, 2> PackagePaths{
    "cod4x1khz.exe", "runtime/cod4x1khz_server.exe"};
inline constexpr std::array<const char*, 4> InstalledPaths{
    PackagePaths[0], PackagePaths[1], "runtime/cod4x1khz.exe", "runtime/cod4x1khz_bench.exe"};
inline constexpr std::array<unsigned, 4> InstalledSourceIndexes{0, 1, 0, 0};
#endif
inline constexpr char StorageDirectory[] = KHZ_UPDATE_DIRECTORY;
using PublicKey = std::array<std::uint8_t, 283>; // CNG RSA2048 public blob, exponent65537.
using Signature = std::array<std::uint8_t, 256>;
struct Policy {
    PublicKey trustedKey{};
    char manifestUrl[1024]{}, origin[256]{}; // Empty means unconfigured/disabled.
    bool privateHttpFixture = false; // Test-controls only; production HTTPS.
};
struct File { char name[64]{}, url[1024]{}; std::uint64_t bytes = 0; content::Digest sha256{}; };
struct Manifest { std::uint64_t version = 0; std::array<File, 2> files{}; Signature signature{}; };
struct Result { bool applied = false, rolledBack = false, recoverable = false; char detail[160]{}; };
bool Configured(const Policy& policy);
bool CanonicalBody(const Manifest& manifest, std::string* body);
bool VerifySignature(std::string_view body, const Signature& signature, const PublicKey& trustedKey);
bool VerifyManifest(std::string_view document, const Policy& policy, std::uint64_t installedVersion, Manifest* result);
bool FetchManifest(const Policy& policy, std::uint64_t installedVersion, Manifest* result);
const char* BinaryIdentity();
bool VerifyBinary(const std::filesystem::path& file);
enum class RecordedState { None, Healthy, RecoveryRequired, Invalid };
// Read-only startup verification: never creates storage, writes version state
// or replaces images. Healthy committed/rolled-back journals allow an ordinary
// game or managed server startup without waiting for other running games.
RecordedState InspectRecordedState(const std::filesystem::path& projectRoot,const Policy& policy);
// All functions below are stopped-process/helper service work. They never run
// inside a game tick. Journal + per-file atomic renames form a recoverable
// four-path transaction; root startup MUST call Recover before executing game
// code. They do not represent a single filesystem-wide atomic rename.
// Root client and runtime server must exist. Optional missing client mirrors
// are created from signed client bytes and removed again on rollback.
Result Apply(const std::filesystem::path& projectRoot, const Manifest& manifest, const Policy& policy,
    const std::array<std::filesystem::path, 2>& stagedFiles, std::uint64_t installedVersion,
    unsigned fixtureFailAfterReplacement = 0);
Result Recover(const std::filesystem::path& projectRoot, const Policy& policy);
Result Rollback(const std::filesystem::path& projectRoot, const Policy& policy);
}
