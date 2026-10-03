#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

// Service/control owner only. Submit, commit and configuration are lifecycle
// work. The worker never accesses CL/CG, zones, dvars, Cbuf or gameplay state.
namespace khz::content {
using Digest = std::array<std::uint8_t, 32>;
inline constexpr unsigned QueueCapacity = 16, CompletionCapacity = 32;
inline constexpr std::uint64_t MaxFileBytes = 1024ull * 1024 * 1024;
enum class Status : unsigned { Staged, Cancelled, Invalid, Network, Storage, Size, Hash, Pure, QueueFull };
struct Request {
    std::uint64_t id = 0, session = 0, generation = 0, expectedBytes = 0;
    Digest sha256{};
    char url[1024]{}, origin[256]{}, relative[240]{};
    bool checkPure = false;
    bool stagingOnly = false; // Signed update blobs never enter the asset FS.
    std::uint32_t checksum = 0, pureChecksum = 0, checksumFeed = 0;
};
struct Completion {
    Request request{};
    Status status = Status::Invalid;
    unsigned attempts = 0, nativeError = 0;
    std::uint64_t bytes = 0, cancellationSerial = 0;
    char detail[128]{};
};
struct Progress { std::uint64_t id = 0, bytes = 0, total = 0; unsigned queued = 0; bool running = false; };

bool SafeAssetPath(std::string_view relative);
bool ApprovedUrl(std::string_view url, std::string_view configuredOrigin);
std::uint32_t Crc32(const void* data, std::size_t length, std::uint32_t initial = 0);
bool Sha256(const void* bytes, std::size_t length, Digest* result);
bool HashFile(const std::filesystem::path& file, Digest* result, std::uint64_t* bytes = nullptr);
bool VerifyIwd(const std::filesystem::path& file, std::uint32_t checksumFeed,
    std::uint32_t* checksum, std::uint32_t* pureChecksum);
bool ConfinedPath(const std::filesystem::path& root, std::string_view relative,
    std::filesystem::path* result, bool createParents = false);
bool AtomicWrite(const std::filesystem::path& file, const void* bytes, std::size_t length);

bool Start(const std::filesystem::path& assetHome, const std::filesystem::path& stagingRoot);
bool Submit(const Request& request); // Requires a configured origin and accepted session/generation.
bool SubmitVerifiedBlob(const Request& request); // Hash/size from an already verified signed manifest.
// Blocking SERVICE-thread call for small signed-manifest documents. Caller
// verifies the signature before interpreting/installing any referenced file.
bool ReadDocument(std::string_view url, std::string_view configuredOrigin,
    std::vector<std::uint8_t>* bytes, unsigned maximum = 65536);
void Cancel(std::uint64_t session, std::uint64_t generation);
bool Poll(Completion* completion);
bool Discard(const Completion& completion); // Service/control cleanup of its owned staging file.
Progress GetProgress();
bool StagedPath(const Completion& completion, std::uint64_t acceptedSession, std::uint64_t generation,
    std::filesystem::path* result);
// Caller must park/disconnect the map/filesystem first. Rechecks bytes/hash and
// pure contract; keeps replaced files in a unique backup. Native FS restart and
// pure handshake remain the runtime adapter's responsibility after success.
bool Commit(const Completion& completion, std::uint64_t acceptedSession, std::uint64_t generation,
    std::filesystem::path* backup = nullptr);
void Stop();
}
