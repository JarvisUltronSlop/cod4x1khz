#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace khz::browser {
inline constexpr unsigned MaxServers = 1024, MaxFavorites = 256, MaxMasterPacket = 8192;
inline constexpr unsigned MaxLocalServers = 128;
struct Endpoint {
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0; // Host order.
    bool operator==(const Endpoint&) const = default;
};
struct Server {
    Endpoint address{};
    char hostname[96]{}, map[64]{}, gameType[32]{}, mod[96]{};
    unsigned clients = 0, maximum = 0, protocol = 0, pingMs = 0;
    bool pure = false, password = false, compatible = false;
};
struct Favorite { Endpoint address{}; char label[96]{}; };
bool ParseEndpoint(std::string_view text, Endpoint* result);
bool FormatEndpoint(const Endpoint& endpoint, char* text, std::size_t capacity);
bool PrivateEndpoint(const Endpoint& endpoint);
bool ParseInfo(std::string_view packet, const Endpoint& source, const Endpoint& expected,
    std::string_view challenge, Server* result);
bool ParseMaster(const void* packet, std::size_t length, const Endpoint& source, const Endpoint& configuredMaster,
    std::array<Endpoint, MaxServers>* servers, unsigned* count, bool* terminal);

// Blocking service-thread UDP calls use a separate socket, absolute timeout,
// strict source/port filtering and a random challenge; no engine packet parser.
bool Query(const Endpoint& endpoint, Server* result, unsigned timeoutMilliseconds = 1000);
bool QueryMaster(const Endpoint& configuredMaster, std::array<Endpoint, MaxServers>* servers,
    unsigned* count, unsigned timeoutMilliseconds = 1000);
// Explicit private LAN/loopback discovery. Random challenge, private sources,
// queried ports only, bounded results and absolute timeout; no public master.
bool Discover(std::array<Server, MaxLocalServers>* servers, unsigned* count,
    unsigned timeoutMilliseconds = 300, std::uint16_t firstPort = 28960, unsigned portCount = 4);
class Favorites {
public:
    bool Add(const Endpoint& endpoint, std::string_view label);
    bool Remove(const Endpoint& endpoint);
    bool Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;
    unsigned Count() const { return count_; }
    const Favorite* At(unsigned index) const { return index < count_ ? &values_[index] : nullptr; }
private:
    std::array<Favorite, MaxFavorites> values_{};
    unsigned count_ = 0;
};
}
