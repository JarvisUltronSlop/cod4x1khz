#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <Windows.h>
#include <khz/khz_plugin_dns.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace dns = khz::plugin_dns;
namespace net = khz::plugin_net;
namespace {
unsigned failures = 0, cases = 0;
void check(bool value, const char* name) { ++cases; if (!value) { ++failures; std::printf("FAIL %s\n", name); } }
net::ParsedEndpoint endpoint(const char* name, net::Type family) {
    net::ParsedEndpoint output;
    check(net::ParseNumeric(name, family, &output) != net::NumericResult::Invalid, "parse fixture endpoint");
    return output;
}
bool waitPending(unsigned count) {
    const auto until = GetTickCount64() + 1000;
    while (GetTickCount64() < until) { if (dns::Stats().pending >= count) return true; Sleep(1); }
    return false;
}
bool waitEmpty() {
    const auto until = GetTickCount64() + 2000;
    while (GetTickCount64() < until) { const auto stats = dns::Stats(); if (!stats.pending && !stats.retiring) return true; Sleep(1); }
    return false;
}
}

int main() {
    check(dns::Init(), "initialize actual bounded async DNS");
    check(dns::Init(), "idempotent startup");
    net::Address output{};
    output.type = KHZ_NET_NA_BROADCAST; output.port = 1234; output.sock = 9876;
    const auto sentinel = output;
    const auto localhost = endpoint("localhost:65535", KHZ_NET_NA_IP);
    check(dns::Resolve(0, 1, localhost, &output), "real localhost IPv4 resolution");
    check(output.type == KHZ_NET_NA_IP && output.ip[0] == 127 && output.port == htons(65535) && output.sock == -1, "IPv4 address and unsigned port preserved");
    check(waitEmpty(), "completed DNS request reclaimed");
    bool serialOkay = true;
    for (unsigned index = 0; index < 20; ++index) serialOkay = dns::Resolve(0, 1, localhost, &output) && serialOkay;
    check(serialOkay && dns::Stats().pending == 0 && dns::Stats().retiring == 0,
        "completed serial lookups immediately release per-owner slots");
    check(dns::Resolve(0, 1, endpoint("localhost:32768", KHZ_NET_NA_TCP), &output), "resolve TCP family without opening TCP socket");
    check(output.type == KHZ_NET_NA_TCP && output.port == htons(32768) && output.sock == -1, "TCP family preserved with no connected socket");
    check(waitEmpty(), "TCP-family resolver reclaimed");
    check(dns::Resolve(0, 1, endpoint("localhost", KHZ_NET_NA_UNSPEC), &output), "resolve unspecified localhost family");
    check(output.type == KHZ_NET_NA_IP && output.port == htons(28960), "UNSPEC prefers actual IPv4 result and stock default port");
    check(dns::Resolve(1, 2, endpoint("localhost:91", KHZ_NET_NA_IP6), &output), "resolve localhost genuine IPv6 address");
    check(output.type == KHZ_NET_NA_IP6 && output.ip6[15] == 1 && output.port == htons(91), "IPv6 address family and port preserved");
    check(dns::Resolve(1, 2, endpoint("localhost:92", KHZ_NET_NA_TCP6), &output), "resolve TCP6 without TCP contact");
    check(output.type == KHZ_NET_NA_TCP6 && output.port == htons(92) && output.sock == -1, "TCP6 address family preserved");
    check(waitEmpty(), "IPv6 requests reclaimed");
    check(dns::Resolve(2, 3, endpoint("127.0.0.1:9", KHZ_NET_NA_IP), &output), "resolve numeric IPv4 through bounded provider");
    check(output.ip[0] == 127 && output.port == htons(9), "numeric IPv4 result preserved");
    check(dns::Resolve(2, 3, endpoint("[::1]:9", KHZ_NET_NA_IP6), &output), "resolve numeric IPv6 through bounded provider");
    check(output.type == KHZ_NET_NA_IP6 && output.ip6[15] == 1, "numeric IPv6 result preserved");
    check(dns::Resolve(2, 3, endpoint("[fe80::1%7]:9", KHZ_NET_NA_IP6), &output), "resolve scoped numeric IPv6 without socket contact");
    check(output.type == KHZ_NET_NA_IP6 && output.scope_id == 7 && output.port == htons(9), "parsed IPv6 scope and port preserved");
    output = sentinel;
    auto invalid = localhost; invalid.host.fill('x');
    check(!dns::Resolve(3, 4, invalid, &output), "reject unterminated copied host");
    invalid = localhost; invalid.host[0] = ' '; check(!dns::Resolve(3, 4, invalid, &output), "reject malformed host");
    invalid = localhost; invalid.host[0] = '?'; check(!dns::Resolve(3, 4, invalid, &output), "reject URL punctuation outside parsed host grammar");
    invalid = localhost; invalid.address.scope_id = -1; check(!dns::Resolve(3, 4, invalid, &output), "reject invalid copied scope");
    invalid = localhost; invalid.requestedFamily = KHZ_NET_NA_BAD; check(!dns::Resolve(3, 4, invalid, &output), "reject nonresolvable requested family");
    invalid = localhost; invalid.returnCode = 0; check(!dns::Resolve(3, 4, invalid, &output), "reject incomplete parsed endpoint");
    check(!dns::Resolve(16, 4, localhost, &output) && !dns::Resolve(3, 0, localhost, &output), "reject invalid owner/generation");
    check(!dns::Resolve(3, 4, localhost, nullptr), "reject null destination");
    check(!std::memcmp(&output, &sentinel, sizeof(output)), "rejection leaves caller destination unchanged");
    check(waitEmpty(), "requests empty before bounded concurrent fixture");
    dns::TestDelayResolution(500);
    std::array<bool, 4> resolved{};
    std::array<std::thread, 4> workers;
    for (unsigned index = 0; index < workers.size(); ++index) workers[index] = std::thread([&, index] {
        net::Address address{}; resolved[index] = dns::Resolve(4, 10, localhost, &address);
    });
    check(waitPending(4), "four actual owner requests admitted");
    output = sentinel;
    check(!dns::Resolve(4, 10, localhost, &output), "reject fifth same-owner operation without waiting");
    check(!std::memcmp(&output, &sentinel, sizeof(output)), "bounded rejection preserves output");
    for (auto& worker : workers) worker.join();
    check(resolved[0] && resolved[1] && resolved[2] && resolved[3], "admitted operations complete successfully");
    check(waitEmpty(), "bounded concurrent operations reclaimed");
    std::atomic<bool> cancelledResult{true};
    std::thread cancelled([&] { net::Address address = sentinel; const bool okay = dns::Resolve(5, 20, localhost, &address);
        cancelledResult.store(okay || std::memcmp(&address, &sentinel, sizeof(address)) != 0); });
    check(waitPending(1), "actual cancelled request was admitted");
    dns::Cancel(5, 20); cancelled.join();
    check(!cancelledResult.load(), "cancelled in-flight request fails without publishing a stale address");
    dns::TestDelayResolution(0);
    output = sentinel;
    check(!dns::Resolve(5, 20, localhost, &output), "cancel tombstone rejects later same-generation request");
    check(dns::Resolve(5, 21, localhost, &output), "higher reload generation can resolve");
    dns::Discard(5, 21);
    check(!dns::Resolve(5, 21, localhost, &output), "discard retires completed generation");
    check(waitEmpty(), "cancel/discard native provider storage reclaimed");
    dns::TestDelayResolution(5500);
    output = sentinel;
    const auto started = GetTickCount64();
    check(!dns::Resolve(6, 30, localhost, &output), "absolute five-second deadline rejects delayed completion");
    check(GetTickCount64() - started >= 4900 && GetTickCount64() - started < 5500, "caller deadline is bounded near five seconds");
    check(!std::memcmp(&output, &sentinel, sizeof(output)), "deadline preserves caller destination");
    dns::TestDelayResolution(0);
    check(waitEmpty(), "deadline request storage reclaimed after provider completion");
    const auto stats = dns::Stats();
    check(stats.completed >= 10 && stats.failed >= 2 && stats.rejected >= 8 && stats.cancelled >= 2, "actual counters include completions, deadlines, rejection and cancellation");
    // This process has joined every backend caller before the host barrier.
    dns::Shutdown();
    check(dns::Stats().pending == 0, "shutdown after caller pin barrier");
    check(!dns::Resolve(0, 1, localhost, &output), "resolve unavailable after shutdown");
    std::printf("Native DNS loopback/numeric/cancellation/deadline: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
