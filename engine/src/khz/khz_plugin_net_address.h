#pragma once
#define KHZ_PUBLIC_NET_NO_SHORT_NAMES
#include <public_net_address.h>
#undef KHZ_PUBLIC_NET_NO_SHORT_NAMES
#include <array>
#include <cstddef>
#include <cstdint>
struct sockaddr;
struct sockaddr_storage;

namespace khz::plugin_net {
using Address = khz_public_netadr_t;
using Type = khz_public_netadrtype_t;
inline constexpr unsigned AddressStringMinimum = 48;
inline constexpr unsigned MaxInterfaces = 64;
enum class Format { Full, Short, Mask };
enum class NumericResult { Invalid, Numeric, NeedsDNS };
struct ParsedEndpoint {
    Address address{};
    std::array<char, 256> host{};
    Type requestedFamily = KHZ_NET_NA_UNSPEC;
    int returnCode = 0; // 1 explicit port, 2 default; Numeric alone is success.
};
struct Interface { Address address{}; std::array<unsigned char, 16> mask{}; };
// An explicit field-copy carrier. Native callers copy native fields into this
// value; they must not reinterpret a native pointer as either public layout.
struct LegacyIPv4 {
    std::int32_t type = 1; unsigned char ip[4]{};
    std::uint16_t port = 0; unsigned char ipx[10]{};
};
static_assert(sizeof(LegacyIPv4) == 20);
bool FromLegacyIPv4(const LegacyIPv4&, Address*);
bool ToLegacyIPv4(const Address&, LegacyIPv4*);
bool CompareBase(const Address*, const Address*, int bits = -1);
bool Compare(const Address*, const Address*);
const char* FormatAddress(const Address*, Format, char* output, int capacity);
NumericResult ParseNumeric(const char* text, Type family, ParsedEndpoint*);
bool IsLAN(const Address*, const Interface* catalog, unsigned count);
bool ToSockaddr(const Address&, sockaddr_storage*, int* length);
bool FromSockaddr(const sockaddr*, int length, bool tcp, std::int32_t socket, Address*);
// Owner lifecycle only: native adapter enumeration is never a tick operation.
// Failure leaves caller output/count unchanged; reject a truncated catalog.
bool CaptureInterfaces(Interface* output, unsigned capacity, unsigned* count);
} // namespace khz::plugin_net
