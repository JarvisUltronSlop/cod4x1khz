#include "khz_plugin_net_address.h"
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace khz::plugin_net {
namespace {
unsigned width(Type type) {
    return type == KHZ_NET_NA_IP || type == KHZ_NET_NA_TCP ? 4 :
        type == KHZ_NET_NA_IP6 || type == KHZ_NET_NA_TCP6 ? 16 : 0;
}
void copy(char* output, int capacity, const char* text) {
    if (!output || capacity <= 0) return;
    const auto count = (std::min)(std::strlen(text), std::size_t(capacity - 1));
    std::memcpy(output, text, count); output[count] = 0;
}
bool number(std::string_view text, unsigned maximum, unsigned* output) {
    if (text.empty()) return false;
    unsigned value = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9' || value > (maximum - unsigned(c - '0')) / 10) return false;
        value = value * 10 + unsigned(c - '0');
    }
    *output = value; return true;
}
bool familyValid(Type family) {
    return family == KHZ_NET_NA_UNSPEC || family == KHZ_NET_NA_IP || family == KHZ_NET_NA_IP6 ||
        family == KHZ_NET_NA_TCP || family == KHZ_NET_NA_TCP6;
}
bool sameBytes(const unsigned char* a, const unsigned char* b, unsigned bytes, unsigned bits) {
    const unsigned whole = bits / 8, remainder = bits % 8;
    if (whole && std::memcmp(a, b, whole)) return false;
    return !remainder || (whole < bytes && ((a[whole] ^ b[whole]) & (0xffu << (8 - remainder))) == 0);
}
}
bool FromLegacyIPv4(const LegacyIPv4& input, Address* output) {
    if (!output) return false;
    Address candidate{};
    switch (input.type) {
    case 0: case 1: candidate.type = KHZ_NET_NA_BAD; break;
    case 2: candidate.type = KHZ_NET_NA_LOOPBACK; break;
    case 3: candidate.type = KHZ_NET_NA_BROADCAST; break;
    case 4: candidate.type = KHZ_NET_NA_IP; break;
    default: *output = {}; return false; // IPX value 5 must NEVER become IPv6.
    }
    std::memcpy(candidate.ip, input.ip, 4); candidate.port = input.port;
    *output = candidate; return true;
}
bool ToLegacyIPv4(const Address& input, LegacyIPv4* output) {
    if (!output) return false;
    LegacyIPv4 candidate{};
    switch (input.type) {
    case KHZ_NET_NA_BAD: candidate.type = 1; break; // Public zero aliases bot/bad.
    case KHZ_NET_NA_LOOPBACK: candidate.type = 2; break;
    case KHZ_NET_NA_BROADCAST: candidate.type = 3; break;
    case KHZ_NET_NA_IP: candidate.type = 4; break;
    default: *output = {}; return false; // IPv6 needs the real transport extension.
    }
    std::memcpy(candidate.ip, input.ip, 4); candidate.port = input.port;
    *output = candidate; return true;
}
bool CompareBase(const Address* a, const Address* b, int bits) {
    if (!a || !b || a->type != b->type) return false;
    if (a->type == KHZ_NET_NA_LOOPBACK) return true;
    const auto bytes = width(a->type); if (!bytes) return false;
    const unsigned count = bits < 0 || unsigned(bits) > bytes * 8 ? bytes * 8 : unsigned(bits);
    return sameBytes(a->ip6, b->ip6, bytes, count);
}
bool Compare(const Address* a, const Address* b) {
    return CompareBase(a, b) && (a->type == KHZ_NET_NA_LOOPBACK || a->port == b->port);
}
bool ToSockaddr(const Address& input, sockaddr_storage* output, int* length) {
    if (!output || !length || input.scope_id < 0) return false;
    sockaddr_storage candidate{};
    if (input.type == KHZ_NET_NA_IP || input.type == KHZ_NET_NA_TCP || input.type == KHZ_NET_NA_BROADCAST) {
        auto& v4 = reinterpret_cast<sockaddr_in&>(candidate); v4.sin_family = AF_INET; v4.sin_port = input.port;
        if (input.type == KHZ_NET_NA_BROADCAST) v4.sin_addr.s_addr = INADDR_BROADCAST;
        else std::memcpy(&v4.sin_addr, input.ip, 4);
        *length = sizeof(v4);
    } else if (input.type == KHZ_NET_NA_IP6 || input.type == KHZ_NET_NA_TCP6 || input.type == KHZ_NET_NA_MULTICAST6) {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(candidate); v6.sin6_family = AF_INET6; v6.sin6_port = input.port;
        v6.sin6_scope_id = std::uint32_t(input.scope_id); std::memcpy(&v6.sin6_addr, input.ip6, 16);
        *length = sizeof(v6);
    } else return false;
    *output = candidate; return true;
}
bool FromSockaddr(const sockaddr* input, int length, bool tcp, std::int32_t socket, Address* output) {
    if (!input || !output || length < int(sizeof(input->sa_family))) return false;
    Address candidate{}; candidate.sock = socket;
    if (input->sa_family == AF_INET && length >= int(sizeof(sockaddr_in))) {
        const auto& v4 = *reinterpret_cast<const sockaddr_in*>(input);
        candidate.type = tcp ? KHZ_NET_NA_TCP : KHZ_NET_NA_IP; candidate.port = v4.sin_port;
        std::memcpy(candidate.ip, &v4.sin_addr, 4);
    } else if (input->sa_family == AF_INET6 && length >= int(sizeof(sockaddr_in6))) {
        const auto& v6 = *reinterpret_cast<const sockaddr_in6*>(input);
        if (v6.sin6_scope_id > INT32_MAX) return false;
        candidate.type = tcp ? KHZ_NET_NA_TCP6 : KHZ_NET_NA_IP6; candidate.port = v6.sin6_port;
        candidate.scope_id = std::int32_t(v6.sin6_scope_id); std::memcpy(candidate.ip6, &v6.sin6_addr, 16);
    } else return false;
    *output = candidate; return true;
}
const char* FormatAddress(const Address* input, Format format, char* output, int capacity) {
    if (!output || capacity <= 0) return output;
    if (capacity < int(AddressStringMinimum)) { copy(output, capacity, "buf < NET_ADDRSTRMAXLEN"); return output; }
    if (!input) return "(null)"; // Pinned return pointer contract, not a copied buffer.
    output[0] = 0;
    if (input->type == KHZ_NET_NA_LOOPBACK) { copy(output, capacity, "loopback"); return output; }
    if (input->type == KHZ_NET_NA_BOT) { copy(output, capacity, "bot"); return output; }
    const auto bytes = width(input->type); if (!bytes || input->scope_id < 0) return output;
    if (format == Format::Mask) {
        if (bytes == 4) std::snprintf(output, capacity, "%u.%u.x.x:%u", unsigned(input->ip[0]), unsigned(input->ip[1]), unsigned(ntohs(input->port)));
        else std::snprintf(output, capacity, "[%02x%02x:%02x%02x:%02x%02x:x:x:x:x:x]:%u",
            unsigned(input->ip6[0]), unsigned(input->ip6[1]), unsigned(input->ip6[2]),
            unsigned(input->ip6[3]), unsigned(input->ip6[4]), unsigned(input->ip6[5]), unsigned(ntohs(input->port)));
        return output;
    }
    char host[80]{};
    if (!InetNtopA(bytes == 4 ? AF_INET : AF_INET6, const_cast<unsigned char*>(input->ip6), host, sizeof(host))) return output;
    if (bytes == 16 && input->scope_id) {
        const auto used = std::strlen(host); std::snprintf(host + used, sizeof(host) - used, "%%%u", unsigned(input->scope_id));
    }
    if (format == Format::Short) copy(output, capacity, host);
    else std::snprintf(output, capacity, bytes == 4 ? "%s:%u" : "[%s]:%u", host, unsigned(ntohs(input->port)));
    return output;
}
NumericResult ParseNumeric(const char* text, Type family, ParsedEndpoint* output) {
    if (!output) return NumericResult::Invalid;
    *output = {}; if (!text || !familyValid(family)) return NumericResult::Invalid;
    unsigned size = 0; while (size < 256 && text[size]) ++size;
    if (!size || size == 256) return NumericResult::Invalid;
    std::string_view host(text, size), port; bool bracketed = false;
    if (host.front() == '[') {
        bracketed = true; const auto closing = host.find(']');
        if (closing == std::string_view::npos || closing < 2) return NumericResult::Invalid;
        if (closing + 1 != host.size()) {
            if (host[closing + 1] != ':') return NumericResult::Invalid;
            port = host.substr(closing + 2); if (port.empty()) return NumericResult::Invalid;
        }
        host = host.substr(1, closing - 1);
    } else {
        const auto colon = host.find(':');
        if (colon != std::string_view::npos && colon == host.rfind(':')) {
            port = host.substr(colon + 1); host = host.substr(0, colon); if (port.empty()) return NumericResult::Invalid;
        }
    }
    if (host.empty()) return NumericResult::Invalid;
    for (unsigned char c : host) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '.' || c == ':' || c == '%' || c == '-' || c == '_')) return NumericResult::Invalid;
    unsigned parsedPort = 28960; if (!port.empty() && !number(port, 65535, &parsedPort)) return NumericResult::Invalid;
    unsigned scope = 0; const auto percent = host.find('%');
    if (percent != std::string_view::npos) {
        if (!number(host.substr(percent + 1), INT32_MAX, &scope)) return NumericResult::Invalid;
        host = host.substr(0, percent); if (host.empty()) return NumericResult::Invalid;
    }
    ParsedEndpoint candidate{}; candidate.requestedFamily = family; candidate.returnCode = port.empty() ? 2 : 1;
    candidate.address.port = htons(std::uint16_t(parsedPort)); candidate.address.scope_id = std::int32_t(scope);
    std::memcpy(candidate.host.data(), host.data(), host.size());
    const bool ipv6Syntax = host.find(':') != std::string_view::npos;
    if (bracketed && !ipv6Syntax) return NumericResult::Invalid;
    const bool want6 = family == KHZ_NET_NA_IP6 || family == KHZ_NET_NA_TCP6 || (family == KHZ_NET_NA_UNSPEC && ipv6Syntax);
    if ((!want6 && (ipv6Syntax || percent != std::string_view::npos)) || (percent != std::string_view::npos && !ipv6Syntax)) return NumericResult::Invalid;
    char numeric[280]{};
    if (want6 && !ipv6Syntax) std::snprintf(numeric, sizeof(numeric), "::ffff:%s", candidate.host.data());
    else std::memcpy(numeric, candidate.host.data(), host.size() + 1);
    if (InetPtonA(want6 ? AF_INET6 : AF_INET, numeric, candidate.address.ip6) != 1) {
        // No resolver is called. Hostnames get a copied bounded request for an
        // explicitly pinned DNS worker; invalid numeric literals stay invalid.
        if (ipv6Syntax || bracketed || percent != std::string_view::npos) return NumericResult::Invalid;
        bool onlyDigitsAndDots = true;
        for (unsigned char c : host) onlyDigitsAndDots = onlyDigitsAndDots && ((c >= '0' && c <= '9') || c == '.');
        if (onlyDigitsAndDots) return NumericResult::Invalid;
        *output = candidate; return NumericResult::NeedsDNS;
    }
    candidate.address.type = want6 ? (family == KHZ_NET_NA_TCP6 ? KHZ_NET_NA_TCP6 : KHZ_NET_NA_IP6) :
        (family == KHZ_NET_NA_TCP ? KHZ_NET_NA_TCP : KHZ_NET_NA_IP);
    *output = candidate; return NumericResult::Numeric;
}
bool IsLAN(const Address* address, const Interface* catalog, unsigned count) {
    if (!address || count > MaxInterfaces || (count && !catalog)) return false;
    if (address->type == KHZ_NET_NA_LOOPBACK) return true;
    const auto bytes = width(address->type); if (!bytes) return false;
    if (bytes == 4) {
        if (address->ip[0] == 10 || address->ip[0] == 127 ||
            (address->ip[0] == 172 && (address->ip[1] & 0xf0) == 16) ||
            (address->ip[0] == 192 && address->ip[1] == 168)) return true;
    } else if ((address->ip6[0] == 0xfe && (address->ip6[1] & 0xc0) == 0x80) || (address->ip6[0] & 0xfe) == 0xfc) return true;
    for (unsigned i = 0; i != count; ++i) if (catalog[i].address.type == address->type) {
        bool match = true;
        for (unsigned b = 0; b != bytes; ++b) if (((address->ip6[b] ^ catalog[i].address.ip6[b]) & catalog[i].mask[b]) != 0) { match = false; break; }
        if (match) return true;
    }
    return false;
}
bool CaptureInterfaces(Interface* output, unsigned capacity, unsigned* count) {
    if (!output || !count || !capacity || capacity > MaxInterfaces) return false;
    // A fixed bounded startup/lifecycle scratch buffer; no tick allocation or
    // DNS. Do not silently accept a partial catalog if the OS needs more space.
    alignas(IP_ADAPTER_ADDRESSES) std::array<unsigned char, 64 * 1024> scratch{};
    ULONG bytes = ULONG(scratch.size());
    const auto result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(scratch.data()), &bytes);
    if (result != ERROR_SUCCESS) return false;
    std::array<Interface, MaxInterfaces> candidate{}; unsigned size = 0;
    for (auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(scratch.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (const auto* node = adapter->FirstUnicastAddress; node; node = node->Next) {
            Interface value{};
            if (!FromSockaddr(node->Address.lpSockaddr, node->Address.iSockaddrLength, false, 0, &value.address)) continue;
            const unsigned addressBits = value.address.type == KHZ_NET_NA_IP ? 32 : 128;
            if (node->OnLinkPrefixLength > addressBits || size == capacity) return false;
            unsigned remaining = node->OnLinkPrefixLength;
            for (unsigned b = 0; b < addressBits / 8; ++b) {
                const unsigned bits = (std::min)(remaining, 8u); remaining -= bits;
                value.mask[b] = bits ? static_cast<unsigned char>(0xffu << (8 - bits)) : 0;
            }
            candidate[size++] = value;
        }
    }
    std::copy_n(candidate.begin(), size, output); *count = size; return true;
}
} // namespace khz::plugin_net
