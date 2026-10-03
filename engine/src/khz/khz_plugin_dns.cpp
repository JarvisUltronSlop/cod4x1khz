#include "khz_plugin_dns.h"
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

namespace khz::plugin_dns {
namespace {
enum class Phase : unsigned { Idle, Caller, Retiring };
struct Resolver {
    OVERLAPPED overlapped{};
    HANDLE cancellation = nullptr;
    PADDRINFOEXW addresses = nullptr;
    wchar_t host[256]{}, service[6]{};
    ADDRINFOEXW hints{};
    timeval timeout{};
    bool pending = false, completed = false, cancelled = false;
    int result = WSA_IO_PENDING;
};
struct Slot {
    std::atomic<Phase> phase{Phase::Idle};
    // Claimed exclusively by the resolver caller; retirement sees these only
    // after that caller publishes Retiring with release ordering.
    Resolver* resolver = nullptr;
};
struct State {
    std::array<Slot, MaxOwners * OperationsPerOwner> slots;
    std::array<std::atomic<std::uint64_t>, MaxOwners> retired{};
    std::atomic<bool> stopping{false}, done{false};
    std::atomic<std::uint64_t> submitted{0}, completed{0}, failed{0}, cancelled{0}, rejected{0};
    HANDLE wake = nullptr;
    std::thread retireThread;
};
std::atomic<State*> current{nullptr};
std::mutex lifecycle;
#ifdef KHZ_PLUGIN_DNS_TESTS
std::atomic<unsigned> testDelay{0};
#endif
void maximum(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    auto previous = target.load(std::memory_order_acquire);
    while (previous < value && !target.compare_exchange_weak(previous, value, std::memory_order_acq_rel)) {}
}
bool cancelled(State& state, unsigned owner, std::uint64_t generation) {
    return state.stopping.load(std::memory_order_acquire) || state.retired[owner].load(std::memory_order_acquire) >= generation;
}
bool familyValid(plugin_net::Type type) {
    return type == KHZ_NET_NA_UNSPEC || type == KHZ_NET_NA_IP || type == KHZ_NET_NA_IP6 ||
        type == KHZ_NET_NA_TCP || type == KHZ_NET_NA_TCP6;
}
bool endpointValid(const plugin_net::ParsedEndpoint& endpoint) {
    const auto length = strnlen(endpoint.host.data(), endpoint.host.size());
    if (!length || length == endpoint.host.size() || !familyValid(endpoint.requestedFamily) ||
        (endpoint.returnCode != 1 && endpoint.returnCode != 2) || endpoint.address.scope_id < 0) return false;
    for (unsigned index = 0; index < length; ++index) {
        const auto value = static_cast<unsigned char>(endpoint.host[index]);
        // This is the copied ParseNumeric host, with brackets, service and
        // numeric scope already removed. Never reinterpret URL punctuation.
        if (!((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') || value == '.' || value == ':' || value == '-' || value == '_')) return false;
    }
    return true;
}
bool resolverComplete(Resolver& resolver) {
    if (resolver.completed) return true;
    if (!resolver.pending || WaitForSingleObject(resolver.overlapped.hEvent, 0) == WAIT_OBJECT_0) {
        resolver.result = resolver.pending ? GetAddrInfoExOverlappedResult(&resolver.overlapped) : resolver.result;
        resolver.completed = true;
        return true;
    }
    return false;
}
void cancelResolver(Resolver& resolver) {
    if (resolver.pending && !resolver.completed && !resolver.cancelled) {
        resolver.cancelled = true;
        if (resolver.cancellation) GetAddrInfoExCancel(&resolver.cancellation);
    }
}
void freeResolver(Resolver* resolver) {
    if (!resolver) return;
    if (resolver->addresses) FreeAddrInfoExW(resolver->addresses);
    if (resolver->overlapped.hEvent) CloseHandle(resolver->overlapped.hEvent);
    delete resolver;
}
void retire(State* state) {
    for (;;) {
        bool empty = true;
        for (auto& slot : state->slots) {
            const auto phase = slot.phase.load(std::memory_order_acquire);
            if (phase == Phase::Idle) continue;
            empty = false;
            if (phase != Phase::Retiring) continue;
            if (slot.resolver) {
                cancelResolver(*slot.resolver);
                if (!resolverComplete(*slot.resolver)) continue;
                freeResolver(slot.resolver); slot.resolver = nullptr;
            }
            slot.phase.store(Phase::Idle, std::memory_order_release);
        }
        if (state->stopping.load(std::memory_order_acquire) && empty) break;
        WaitForSingleObject(state->wake, 10);
    }
    state->done.store(true, std::memory_order_release);
}
void dispose(State* state) {
    if (state->retireThread.joinable()) state->retireThread.join();
    CloseHandle(state->wake); delete state; WSACleanup();
}
bool selectAddress(const Resolver& resolver, const plugin_net::ParsedEndpoint& endpoint, plugin_net::Address* output) {
    const bool tcp = endpoint.requestedFamily == KHZ_NET_NA_TCP || endpoint.requestedFamily == KHZ_NET_NA_TCP6;
    for (unsigned pass = 0; pass < (endpoint.requestedFamily == KHZ_NET_NA_UNSPEC ? 2u : 1u); ++pass) {
        const int desired = endpoint.requestedFamily == KHZ_NET_NA_IP6 || endpoint.requestedFamily == KHZ_NET_NA_TCP6 || pass ? AF_INET6 : AF_INET;
        unsigned inspected = 0;
        for (auto* address = resolver.addresses; address && inspected++ < 64; address = address->ai_next) {
            if (address->ai_family != desired || address->ai_addrlen > INT_MAX) continue;
            plugin_net::Address candidate{};
            if (!plugin_net::FromSockaddr(address->ai_addr, static_cast<int>(address->ai_addrlen), tcp, -1, &candidate)) continue;
            candidate.port = endpoint.address.port;
            if (desired == AF_INET6 && endpoint.address.scope_id) candidate.scope_id = endpoint.address.scope_id;
            *output = candidate; return true;
        }
    }
    return false;
}
}

bool Init() {
    std::lock_guard<std::mutex> lock(lifecycle);
    if (auto* state = current.load(std::memory_order_acquire)) {
        if (!state->stopping.load()) return true;
        if (!state->done.load()) return false;
        current = nullptr; dispose(state);
    }
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
    auto* state = new(std::nothrow) State;
    if (!state) { WSACleanup(); return false; }
    state->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->wake) { delete state; WSACleanup(); return false; }
    try { state->retireThread = std::thread(retire, state); }
    catch (...) { CloseHandle(state->wake); delete state; WSACleanup(); return false; }
    current.store(state, std::memory_order_release); return true;
}
bool Resolve(unsigned owner, std::uint64_t generation, const plugin_net::ParsedEndpoint& endpoint, plugin_net::Address* output) {
    auto* state = current.load(std::memory_order_acquire);
    if (!state || state->stopping.load()) return false;
    if (owner >= MaxOwners || !generation || !output || !endpointValid(endpoint) || cancelled(*state, owner, generation)) {
        ++state->rejected; return false;
    }
    Slot* selected = nullptr;
    for (unsigned index = 0; index < OperationsPerOwner; ++index) {
        auto& slot = state->slots[owner * OperationsPerOwner + index];
        auto expected = Phase::Idle;
        if (slot.phase.compare_exchange_strong(expected, Phase::Caller, std::memory_order_acq_rel)) { selected = &slot; break; }
    }
    if (!selected) { ++state->rejected; return false; }
    ++state->submitted;
    const auto deadline = GetTickCount64() + DeadlineMilliseconds;
    auto* resolver = new(std::nothrow) Resolver;
    bool success = false;
    plugin_net::Address candidate{};
    if (resolver) {
        resolver->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (unsigned index = 0; endpoint.host[index]; ++index) resolver->host[index] = static_cast<unsigned char>(endpoint.host[index]);
        _snwprintf_s(resolver->service, _TRUNCATE, L"%u", unsigned(ntohs(endpoint.address.port)));
        resolver->hints.ai_family = endpoint.requestedFamily == KHZ_NET_NA_UNSPEC ? AF_UNSPEC :
            endpoint.requestedFamily == KHZ_NET_NA_IP6 || endpoint.requestedFamily == KHZ_NET_NA_TCP6 ? AF_INET6 : AF_INET;
        const bool tcp = endpoint.requestedFamily == KHZ_NET_NA_TCP || endpoint.requestedFamily == KHZ_NET_NA_TCP6;
        resolver->hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;
        resolver->hints.ai_protocol = tcp ? IPPROTO_TCP : IPPROTO_UDP;
        resolver->hints.ai_flags = AI_NUMERICSERV;
        resolver->timeout.tv_sec = DeadlineMilliseconds / 1000;
        if (resolver->overlapped.hEvent) {
            resolver->result = GetAddrInfoExW(resolver->host, resolver->service, NS_ALL, nullptr, &resolver->hints,
                &resolver->addresses, &resolver->timeout, &resolver->overlapped, nullptr, &resolver->cancellation);
            resolver->pending = resolver->result == WSA_IO_PENDING; resolver->completed = !resolver->pending;
#ifdef KHZ_PLUGIN_DNS_TESTS
            const auto delayedUntil = GetTickCount64() + testDelay.load();
            while (GetTickCount64() < delayedUntil && GetTickCount64() < deadline && !cancelled(*state, owner, generation)) Sleep(1);
#endif
            while (!resolverComplete(*resolver) && GetTickCount64() < deadline && !cancelled(*state, owner, generation))
                WaitForSingleObject(resolver->overlapped.hEvent, 5);
            if (resolverComplete(*resolver) && resolver->result == 0 && GetTickCount64() < deadline && !cancelled(*state, owner, generation))
                success = selectAddress(*resolver, endpoint, &candidate);
            if (!resolver->completed) cancelResolver(*resolver);
        } else { resolver->completed = true; resolver->result = WSA_NOT_ENOUGH_MEMORY; }
    }
    // Publish copied output only after the final generation check. An already
    // validated return may race a later Cancel; the host rechecks its pin.
    success = success && !cancelled(*state, owner, generation);
    if (success) { *output = candidate; ++state->completed; } else ++state->failed;
    if (resolver && resolver->completed) { freeResolver(resolver); resolver = nullptr; }
    selected->resolver = resolver;
    // Complete providers have no storage left to retire. Release their slot
    // immediately so serial localhost lookups cannot fill the per-owner bound.
    selected->phase.store(resolver ? Phase::Retiring : Phase::Idle, std::memory_order_release);
    if (resolver) SetEvent(state->wake);
    return success;
}
void Cancel(unsigned owner, std::uint64_t generation) {
    auto* state = current.load(std::memory_order_acquire);
    if (!state || owner >= MaxOwners || !generation) return;
    maximum(state->retired[owner], generation); ++state->cancelled; SetEvent(state->wake);
}
void Discard(unsigned owner, std::uint64_t generation) { Cancel(owner, generation); }
void Shutdown() {
    std::lock_guard<std::mutex> lock(lifecycle);
    auto* state = current.load(std::memory_order_acquire); if (!state) return;
    state->stopping.store(true, std::memory_order_release); SetEvent(state->wake);
    const auto deadline = GetTickCount64() + 6000;
    while (!state->done.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
    if (!state->done.load()) return;
    current = nullptr; dispose(state);
}
Statistics Stats() {
    Statistics output; auto* state = current.load(std::memory_order_acquire); if (!state) return output;
    for (const auto& slot : state->slots) {
        switch (slot.phase.load(std::memory_order_acquire)) {
        case Phase::Caller: ++output.pending; break;
        case Phase::Retiring: ++output.retiring; break;
        default: break;
        }
    }
    output.submitted = state->submitted.load(); output.completed = state->completed.load(); output.failed = state->failed.load();
    output.cancelled = state->cancelled.load(); output.rejected = state->rejected.load(); output.stopping = state->stopping.load(); return output;
}
#ifdef KHZ_PLUGIN_DNS_TESTS
void TestDelayResolution(unsigned milliseconds) { testDelay.store(milliseconds); }
#endif
}
