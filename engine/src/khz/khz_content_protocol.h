#pragma once
#include "khz_content.h"
#include <string>
#include <string_view>

// This is unsigned content metadata from an accepted private custom-server
// connection, not public-server authentication or a signed update manifest.
// Transport admission, slot identity and current map/session are caller gates.
namespace khz::content::wire {
inline constexpr unsigned Format = 1, MaximumText = 1023; // Native reliable command includes its terminating NUL.
struct Query {
    std::uint64_t id = 0, session = 0, generation = 0;
    char relative[240]{};
};
struct Metadata {
    Query query{};
    std::uint64_t bytes = 0;
    Digest sha256{};
    std::uint32_t checksum = 0, pureChecksum = 0, checksumFeed = 0;
    bool checkPure = false;
    char url[1024]{};
};
enum class Error : unsigned { Invalid = 1, Disabled, Missing, Denied, Busy, Stale, Storage };
struct Rejection { Query query{}; Error error = Error::Invalid; };
bool ParseQuery(std::string_view text, Query* result);
bool FormatQuery(const Query& query, std::string* result);
bool ParseMetadata(std::string_view text, Metadata* result);
bool FormatMetadata(const Metadata& metadata, std::string* result);
bool ParseRejection(std::string_view text, Rejection* result);
bool FormatRejection(const Rejection& rejection, std::string* result);
// The client selects the origin; a server response cannot configure one.
bool MakeRequest(const Metadata& metadata, const Query& pending,
    std::string_view configuredOrigin, std::uint32_t acceptedChecksumFeed, Request* result);
}
