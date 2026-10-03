#pragma once
#include "khz_plugin_net_address.h"
#include <cstdint>

namespace khz::plugin_dns {
inline constexpr unsigned MaxOwners = 16, OperationsPerOwner = 4, DeadlineMilliseconds = 5000;
bool Init();
// Pinned plugin jobs/threads only. Resolves an actual address, without opening
// a socket. Failure leaves output unchanged. UNSPEC prefers IPv4, then IPv6.
bool Resolve(unsigned owner, std::uint64_t generation,
    const plugin_net::ParsedEndpoint&, plugin_net::Address* output);
// Main-safe constant work: retire this generation, including later requests.
void Cancel(unsigned owner, std::uint64_t generation);
// Host calls only after module callbacks/jobs/threads leave. Results are values,
// so no caller-visible response allocation needs a separate release phase.
void Discard(unsigned owner, std::uint64_t generation);
// Lifecycle barrier: no backend caller (including Stats/Cancel) may remain or
// begin during Shutdown. Retain unfinished provider OVERLAPPED storage rather
// than freeing it if cancellation fails to complete within the bounded wait.
void Shutdown();
struct Statistics {
    unsigned pending = 0, retiring = 0;
    std::uint64_t submitted = 0, completed = 0, failed = 0, cancelled = 0, rejected = 0;
    bool stopping = false;
};
Statistics Stats();
#ifdef KHZ_PLUGIN_DNS_TESTS
void TestDelayResolution(unsigned milliseconds);
#endif
}
