#include <khz/khz_plugin_net_address.h>
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
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
extern "C" int KHZ_PublicNetAddressCSmoke(void);
namespace {
namespace net = khz::plugin_net;
unsigned cases = 0, failures = 0;
void check(bool result, const char* label) { ++cases; if (!result) { ++failures; std::printf("FAIL: %s\n", label); } }
net::Address address(const char* text, net::Type family = KHZ_NET_NA_UNSPEC) {
    net::ParsedEndpoint endpoint; check(net::ParseNumeric(text, family, &endpoint) == net::NumericResult::Numeric, "numeric test address parses"); return endpoint.address;
}
void numeric() {
    check(KHZ_PublicNetAddressCSmoke() == 1, "actual C header layout and public field syntax");
    net::ParsedEndpoint parsed;
    check(net::ParseNumeric("192.0.2.44:65535", KHZ_NET_NA_IP, &parsed) == net::NumericResult::Numeric &&
        parsed.returnCode == 1 && parsed.address.type == KHZ_NET_NA_IP && ntohs(parsed.address.port) == 65535,
        "explicit unsigned IPv4 port and public return 1");
    const unsigned char expected4[] = {192, 0, 2, 44};
    check(!std::memcmp(parsed.address.ip, expected4, 4) && !parsed.address.pad && !parsed.address.scope_id && !parsed.address.sock,
        "golden IPv4 bytes and deterministic reserved fields");
    check(net::ParseNumeric("192.0.2.44", KHZ_NET_NA_TCP, &parsed) == net::NumericResult::Numeric &&
        parsed.returnCode == 2 && parsed.address.type == KHZ_NET_NA_TCP && ntohs(parsed.address.port) == 28960,
        "TCP numeric family and default port return 2");
    check(net::ParseNumeric("[2001:db8:1234:5678:abcd:ef01:2345:6789]:32768", KHZ_NET_NA_IP6, &parsed) == net::NumericResult::Numeric &&
        parsed.returnCode == 1 && ntohs(parsed.address.port) == 32768 && parsed.address.type == KHZ_NET_NA_IP6,
        "IPv6 bracket syntax and signed-boundary port");
    const unsigned char expected6[] = {0x20, 1, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78, 0xab, 0xcd, 0xef, 1, 0x23, 0x45, 0x67, 0x89};
    check(!std::memcmp(parsed.address.ip6, expected6, 16), "independent full IPv6 golden bytes");
    in6_addr native{}; check(InetPtonA(AF_INET6, "2001:db8:1234:5678:abcd:ef01:2345:6789", &native) == 1 &&
        !std::memcmp(parsed.address.ip6, &native, 16), "numeric IPv6 agrees with native Winsock decoder");
    check(net::ParseNumeric("fe80::1%17", KHZ_NET_NA_TCP6, &parsed) == net::NumericResult::Numeric &&
        parsed.address.type == KHZ_NET_NA_TCP6 && parsed.address.scope_id == 17 && parsed.returnCode == 2,
        "Windows numeric scope retained with TCP6 family");
    check(net::ParseNumeric("127.0.0.1:0", KHZ_NET_NA_IP6, &parsed) == net::NumericResult::Numeric &&
        parsed.address.ip6[10] == 255 && parsed.address.ip6[11] == 255 && parsed.address.ip6[12] == 127 &&
        parsed.address.ip6[15] == 1 && !parsed.address.port, "forced IPv6 maps IPv4 literal and preserves explicit port zero");
    check(net::ParseNumeric("localhost:28960", KHZ_NET_NA_UNSPEC, &parsed) == net::NumericResult::NeedsDNS &&
        !std::strcmp(parsed.host.data(), "localhost") && parsed.returnCode == 1 && parsed.address.type == KHZ_NET_NA_BAD,
        "hostname is copied work, never a falsely successful numeric result");
    check(net::ParseNumeric("example.test", KHZ_NET_NA_TCP6, &parsed) == net::NumericResult::NeedsDNS &&
        parsed.requestedFamily == KHZ_NET_NA_TCP6 && parsed.returnCode == 2, "DNS work retains requested family and default port");
    for (const char* text : {"", "[]", "[::1", "[::1]tail", "[127.0.0.1]:80", "[::1]:", "[::1]:65536", "127.0.0.1:-1",
        "127.0.0.1:1x", "127.0.0.1:", "1.2.3.999", "::ffff:300.1.1.1", "http://localhost:80", "a b", "a\nb", "a/b",
        "a@b", "fe80::1%-1", "fe80::1%2147483648", "fe80::1%eth0", "1.2.3.4%1", "::1%", "[::1]:80:81"})
        check(net::ParseNumeric(text, KHZ_NET_NA_UNSPEC, &parsed) == net::NumericResult::Invalid && parsed.returnCode == 0,
            "bounded malformed input rejected before resolver");
    std::string longText(256, 'a');
    check(net::ParseNumeric(longText.c_str(), KHZ_NET_NA_UNSPEC, &parsed) == net::NumericResult::Invalid, "oversize host fails before copying");
    check(net::ParseNumeric("::1", KHZ_NET_NA_IP, &parsed) == net::NumericResult::Invalid &&
        net::ParseNumeric("127.0.0.1", KHZ_NET_NA_LOOPBACK, &parsed) == net::NumericResult::Invalid,
        "requested family mismatch and unsupported family rejected");
}
void comparison() {
    auto a = address("192.0.2.44:32768"), b = a;
    b.port = htons(65535);
    check(net::CompareBase(&a, &b) && !net::Compare(&a, &b), "base ignores port, full compares unsigned port");
    b = a; b.sock = 99; b.scope_id = 7;
    check(net::Compare(&a, &b), "pinned comparison ignores scope and socket");
    b.type = KHZ_NET_NA_TCP; check(!net::CompareBase(&a, &b, 0), "matching bytes in different types never compare equal");
    net::Address loop{}; loop.type = KHZ_NET_NA_LOOPBACK; auto other = loop; other.port = htons(2);
    check(net::Compare(&loop, &other), "loopback ignores port");
    net::Address bad{}; check(!net::CompareBase(&bad, &bad) && !net::CompareBase(nullptr, &a) && !net::Compare(&a, nullptr), "bad/null comparison safely fails");
    for (const char* text : {"192.0.2.44", "2001:db8::5678"}) {
        const auto original = address(text); const unsigned width = original.type == KHZ_NET_NA_IP ? 32 : 128;
        for (unsigned bits : {0u, 1u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 127u, 128u}) if (bits <= width) {
            auto changed = original;
            if (bits < width) changed.ip6[bits / 8] ^= static_cast<unsigned char>(0x80u >> (bits % 8));
            check(net::CompareBase(&original, &changed, int(bits)), "bits outside prefix ignored at byte/partial-byte boundaries");
            if (bits) { changed = original; changed.ip6[(bits - 1) / 8] ^= static_cast<unsigned char>(0x80u >> ((bits - 1) % 8));
                check(!net::CompareBase(&original, &changed, int(bits)), "last bit inside prefix must match"); }
        }
        auto changed = original; changed.ip6[width / 8 - 1] ^= 1;
        check(!net::CompareBase(&original, &changed, -1) && !net::CompareBase(&original, &changed, int(width + 1)) &&
            net::CompareBase(&original, &original, INT32_MAX), "negative and excessive masks clamp to full width");
    }
}
void formatting() {
    char text[128]{};
    auto v4 = address("192.0.2.44:65535");
    check(net::FormatAddress(&v4, net::Format::Full, text, sizeof(text)) == text && !std::strcmp(text, "192.0.2.44:65535"), "full IPv4 golden unsigned format");
    net::FormatAddress(&v4, net::Format::Short, text, sizeof(text)); check(!std::strcmp(text, "192.0.2.44"), "IPv4 short omits port");
    net::FormatAddress(&v4, net::Format::Mask, text, sizeof(text)); check(!std::strcmp(text, "192.0.x.x:65535"), "pinned masked IPv4 format");
    auto v6 = address("[2001:db8:1234:5678:abcd:ef01:2345:6789]:32768");
    net::FormatAddress(&v6, net::Format::Full, text, sizeof(text));
    check(!std::strcmp(text, "[2001:db8:1234:5678:abcd:ef01:2345:6789]:32768"), "full IPv6 golden bracketed format");
    net::FormatAddress(&v6, net::Format::Mask, text, sizeof(text));
    check(!std::strcmp(text, "[2001:0db8:1234:x:x:x:x:x]:32768"), "pinned masked IPv6 keeps first 48 bits");
    auto scoped = address("[fe80::1%17]:65535");
    net::FormatAddress(&scoped, net::Format::Full, text, sizeof(text)); check(!std::strcmp(text, "[fe80::1%17]:65535"), "scope retained in full IPv6");
    net::FormatAddress(&scoped, net::Format::Short, text, sizeof(text)); check(!std::strcmp(text, "fe80::1%17"), "scope retained in short IPv6");
    net::Address loop{}; loop.type = KHZ_NET_NA_LOOPBACK;
    net::FormatAddress(&loop, net::Format::Full, text, sizeof(text)); check(!std::strcmp(text, "loopback"), "loopback pinned spelling");
    net::Address bad{}; net::FormatAddress(&bad, net::Format::Short, text, sizeof(text)); check(!std::strcmp(text, "bot"), "public zero bot/bad alias pinned spelling");
    std::strcpy(text, "untouched"); const char* result = net::FormatAddress(nullptr, net::Format::Full, text, sizeof(text));
    check(result && !std::strcmp(result, "(null)") && !std::strcmp(text, "untouched"), "null address returns literal without copying caller buffer");
    std::array<char, 50> bounded{}; bounded.fill('X');
    net::FormatAddress(&v4, net::Format::Full, bounded.data() + 1, 8);
    check(bounded[0] == 'X' && bounded[9] == 'X' && !std::strcmp(bounded.data() + 1, "buf < N"), "short buffer preserves sentinels and pinned error text");
    auto longest = v6; longest.scope_id = INT32_MAX;
    bounded.fill('X'); net::FormatAddress(&longest, net::Format::Full, bounded.data() + 1, 48);
    check(bounded[0] == 'X' && bounded[49] == 'X' && bounded[48] == 0, "minimum buffer truncates safely without overwrite");
    check(net::FormatAddress(&v4, net::Format::Full, nullptr, 48) == nullptr, "null output safely rejected");
    net::Address unsupported{}; unsupported.type = KHZ_NET_NA_DOWN; std::strcpy(text, "old");
    net::FormatAddress(&unsupported, net::Format::Full, text, sizeof(text)); check(!text[0], "unsupported address clears stale formatter text");
}
void bridge() {
    net::LegacyIPv4 legacy{}; legacy.type = 4; const unsigned char bytes[] = {203, 0, 113, 5};
    std::memcpy(legacy.ip, bytes, 4); legacy.port = htons(65535);
    net::Address publicAddress{};
    check(net::FromLegacyIPv4(legacy, &publicAddress) && publicAddress.type == KHZ_NET_NA_IP &&
        !std::memcmp(publicAddress.ip, bytes, 4) && publicAddress.port == legacy.port && !publicAddress.scope_id && !publicAddress.sock,
        "explicit native field copy yields correct public offsets");
    net::LegacyIPv4 restored{};
    check(net::ToLegacyIPv4(publicAddress, &restored) && !std::memcmp(&restored, &legacy, sizeof(legacy)), "explicit IPv4 legacy roundtrip");
    legacy.type = 5; check(!net::FromLegacyIPv4(legacy, &publicAddress), "native IPX enum must not become public IPv6");
    const auto v6 = address("::1"); check(!net::ToLegacyIPv4(v6, &restored), "IPv6 never truncated into legacy IPv4 transport");
    net::Address bad{}; check(net::ToLegacyIPv4(bad, &restored) && restored.type == 1, "ambiguous public zero translates to native bad, never native bot injection");
    for (const char* text : {"192.0.2.5:65535", "[fe80::1234%7]:32768"}) {
        auto original = address(text); original.sock = 321;
        sockaddr_storage storage{}; int length = 0;
        check(net::ToSockaddr(original, &storage, &length), "actual native sockaddr conversion");
        net::Address roundtrip{};
        check(net::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length, false, 321, &roundtrip) &&
            !std::memcmp(&original, &roundtrip, sizeof(original)), "actual sockaddr preserves all address/port/scope/socket fields");
        check(!net::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length - 1, false, 321, &roundtrip), "short sockaddr rejected before field access");
        check(net::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length, true, 321, &roundtrip) &&
            roundtrip.type == (original.type == KHZ_NET_NA_IP ? KHZ_NET_NA_TCP : KHZ_NET_NA_TCP6), "native TCP family conversion remains explicit");
    }
    auto broadcast = address("192.0.2.5"); broadcast.type = KHZ_NET_NA_BROADCAST;
    sockaddr_storage storage{}; int length = 0;
    check(net::ToSockaddr(broadcast, &storage, &length) && reinterpret_cast<sockaddr_in&>(storage).sin_addr.s_addr == INADDR_BROADCAST,
        "broadcast socket destination uses native broadcast bytes");
}
void lan() {
    for (const char* text : {"10.9.8.7", "127.9.8.7", "172.16.0.1", "172.31.255.255", "192.168.255.1", "fe80::1", "febf::1", "fc00::1", "fdff::1"}) {
        auto a = address(text); check(net::IsLAN(&a, nullptr, 0), "pinned private/link-local/ULA LAN prefix");
    }
    for (const char* text : {"172.15.255.255", "172.32.0.1", "192.169.0.1", "192.0.2.1", "2001:db8::1", "fec0::1", "::1"}) {
        auto a = address(text); check(!net::IsLAN(&a, nullptr, 0), "pinned non-LAN without actual interface membership");
    }
    std::array<net::Interface, 2> catalog{};
    catalog[0].address = address("192.0.2.17"); catalog[0].mask[0] = catalog[0].mask[1] = catalog[0].mask[2] = 255;
    catalog[1].address = address("2001:db8:1234::17"); for (unsigned i = 0; i != 6; ++i) catalog[1].mask[i] = 255;
    auto v4 = address("192.0.2.254"), v6 = address("2001:db8:1234:ffff::1"), outside = address("2001:db8:1235::1");
    check(net::IsLAN(&v4, catalog.data(), unsigned(catalog.size())) && net::IsLAN(&v6, catalog.data(), unsigned(catalog.size())) &&
        !net::IsLAN(&outside, catalog.data(), unsigned(catalog.size())), "actual copied interface masks determine network membership");
    v4.type = KHZ_NET_NA_TCP; check(!net::IsLAN(&v4, catalog.data(), unsigned(catalog.size())), "interface catalog retains pinned exact family type matching");
    check(!net::IsLAN(&v6, nullptr, 1) && !net::IsLAN(&v6, catalog.data(), net::MaxInterfaces + 1), "invalid interface catalog rejected without scan");
}
}
int main() {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
    numeric(); comparison(); formatting(); bridge(); lan();
    WSACleanup(); std::printf("{\"cases\":%u,\"failures\":%u}\n", cases, failures); return failures ? 1 : 0;
}
