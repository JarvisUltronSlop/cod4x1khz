// Source-owned address/UDP adapters. Hostname resolution uses the separately
// bounded resolver; native transport stays IPv4 until its actual migration.
namespace {
struct PluginInterfaceCatalog {
    std::array<khz::plugin_net::Interface, khz::plugin_net::MaxInterfaces> values{};
    unsigned count = 0;
    std::atomic<unsigned> readers{0};
};
std::array<PluginInterfaceCatalog, 3> pluginInterfaceCatalogs{};
std::atomic<unsigned> publishedPluginInterfaces{0};
std::atomic<std::uint64_t> pluginNumericDNSPending{0}, pluginInterfacesRejected{0};
std::array<std::atomic<std::uint64_t>, plug::MaxPlugins> pluginNetworkGenerations{};
using PluginPacketMailbox = khz::plugins::PacketMailbox<khz::plugin_net::Address>;
PluginPacketMailbox pluginPackets;
std::atomic<std::uint64_t> pluginNetworkEpoch{1}, pluginNetworkSession{0};
std::atomic<int> pluginNetworkServerId{0};
std::atomic<unsigned> pluginNetworkVersion{0};
std::atomic<std::uint64_t> pluginUdpSent{0}, pluginUdpSendFailed{0}, pluginUdpSendVeto{0},
    pluginUdpReceived{0}, pluginUdpReceiveVeto{0}, pluginUdpInvalid{0},
    pluginInjected{0}, pluginInjectionDelivered{0}, pluginInjectionRejected{0};
unsigned pluginUdpDepth = 0; // native server owner only
struct PluginUdpDepth {
    bool previous = networkCallback;
    PluginUdpDepth() { ++pluginUdpDepth; networkCallback = true; }
    ~PluginUdpDepth() { --pluginUdpDepth; networkCallback = previous; }
};
void pluginNetworkActivate(unsigned plugin, std::uint64_t generation) {
    if (plugin < pluginNetworkGenerations.size()) pluginNetworkGenerations[plugin].store(generation, std::memory_order_release);
}
void pluginNetworkCancel(unsigned plugin, std::uint64_t generation) {
    if (plugin >= pluginNetworkGenerations.size()) return;
    pluginNetworkGenerations[plugin].compare_exchange_strong(generation, 0, std::memory_order_acq_rel);
}
void pluginNetworkInvalidate() {
    owner(); pluginNetworkVersion.fetch_add(1, std::memory_order_acq_rel);
    pluginUdpDepth = 0; networkCallback = false; // native error paths use longjmp
    ++pluginNetworkEpoch; pluginNetworkSession.store(0, std::memory_order_release);
    pluginNetworkServerId.store(0, std::memory_order_release);
    pluginNetworkVersion.fetch_add(1, std::memory_order_release);
}
bool pluginNetworkContext(void* caller, int* plugin, std::uint64_t* generation) {
    if (shuttingDown || !pluginHttpContext(caller, true, plugin, generation) ||
        *plugin < 0 || unsigned(*plugin) >= pluginNetworkGenerations.size()) return false;
    if (pluginNetworkGenerations[*plugin].load(std::memory_order_acquire) != *generation) { ++apiRejected; return false; }
    if (Sys_IsMainThread()) {
        const auto* entry = registry.Get(*plugin);
        if (!entry || (entry->state != plug::State::Enabled && entry->state != plug::State::Initializing)) return false;
    }
    return true;
}
bool pluginNativeDatagramAddress(const khz::plugin_net::Address& address, netadr_t* native, bool receive) {
    if (address.pad || address.scope_id ||
        (receive ? address.type != KHZ_NET_NA_IP && address.type != KHZ_NET_NA_LOOPBACK :
            address.type != KHZ_NET_NA_IP && address.type != KHZ_NET_NA_LOOPBACK && address.type != KHZ_NET_NA_BROADCAST)) return false;
    return nativePluginAddress(&address, native);
}
bool pluginPacketToken(int plugin, std::uint64_t generation, PluginPacketMailbox::Token* token) {
    if (Sys_IsMainThread()) {
        if (!com_sv_running || !com_sv_running->current.enabled || sv.state != SS_GAME) return false;
        *token = {unsigned(plugin), generation, pluginNetworkEpoch.load(std::memory_order_acquire), KHZ_CurrentServerSession(), sv_serverId_value};
        return token->session != 0;
    }
    // Atomic copied publication: workers never read mutable sv/dvar/registry.
    for (unsigned attempt = 0; attempt != 4; ++attempt) {
        const unsigned before = pluginNetworkVersion.load(std::memory_order_acquire);
        if (before & 1) continue;
        PluginPacketMailbox::Token copy{unsigned(plugin), generation,
            pluginNetworkEpoch.load(std::memory_order_acquire), pluginNetworkSession.load(std::memory_order_acquire),
            pluginNetworkServerId.load(std::memory_order_acquire)};
        if (pluginNetworkVersion.load(std::memory_order_acquire) == before) { *token = copy; return copy.session != 0; }
    }
    return false;
}
void pluginNetworkService() {
    owner();
    pluginNetworkVersion.fetch_add(1, std::memory_order_acq_rel);
    const auto session = com_sv_running && com_sv_running->current.enabled && sv.state == SS_GAME ? KHZ_CurrentServerSession() : 0;
    pluginNetworkSession.store(session, std::memory_order_release);
    pluginNetworkServerId.store(sv_serverId_value, std::memory_order_release);
    pluginNetworkVersion.fetch_add(1, std::memory_order_release);
    // Fixed service budget; all queued bytes are host-owned, not DLL storage.
    static PluginPacketMailbox::Packet packet;
    for (unsigned count = 0; count != 8 && pluginPackets.Pop(packet); ++count) {
        const auto* entry = registry.Get(int(packet.token.owner));
        netadr_t native{};
        if (!session || packet.token.session != session || packet.token.serverId != sv_serverId_value ||
            packet.token.epoch != pluginNetworkEpoch.load(std::memory_order_acquire) || !entry ||
            entry->state != plug::State::Enabled || entry->generation != packet.token.generation ||
            pluginNetworkGenerations[packet.token.owner].load(std::memory_order_acquire) != packet.token.generation ||
            !pluginNativeDatagramAddress(packet.endpoint, &native, true)) { ++pluginInjectionRejected; continue; }
        // Injection deliberately bypasses the physical receive event, matching
        // the public API. Native challenge/proof/protocol validation still runs.
        msg_t message{}; MSG_InitReadOnly(&message, packet.data.data(), int(packet.length));
        SV_PacketEvent(native, &message); ++pluginInjectionDelivered;
    }
}
const char* pluginNetworkStats() {
    const auto dns = khz::plugin_dns::Stats();
    static char text[768];
    std::snprintf(text, sizeof(text), "{\"udp_sent\":%llu,\"udp_send_failed\":%llu,\"send_veto\":%llu,\"received\":%llu,\"receive_veto\":%llu,\"invalid\":%llu,\"injected\":%llu,\"injection_delivered\":%llu,\"injection_rejected\":%llu,\"dns_rejected_main\":%llu,\"interface_rejected\":%llu,\"dns\":{\"pending\":%u,\"retiring\":%u,\"submitted\":%llu,\"completed\":%llu,\"failed\":%llu,\"cancelled\":%llu,\"rejected\":%llu}}",
        pluginUdpSent.load(), pluginUdpSendFailed.load(), pluginUdpSendVeto.load(), pluginUdpReceived.load(),
        pluginUdpReceiveVeto.load(), pluginUdpInvalid.load(), pluginInjected.load(), pluginInjectionDelivered.load(),
        pluginInjectionRejected.load(), pluginNumericDNSPending.load(), pluginInterfacesRejected.load(),
        dns.pending, dns.retiring, dns.submitted, dns.completed, dns.failed, dns.cancelled, dns.rejected);
    return text;
}
bool pluginAddressLAN(const khz::plugin_net::Address* address) {
    for (unsigned attempt = 0; attempt != 4; ++attempt) {
        const unsigned index = publishedPluginInterfaces.load(std::memory_order_acquire);
        auto& frame = pluginInterfaceCatalogs[index]; frame.readers.fetch_add(1, std::memory_order_acq_rel);
        if (publishedPluginInterfaces.load(std::memory_order_acquire) == index) {
            const bool result = khz::plugin_net::IsLAN(address, frame.values.data(), frame.count);
            frame.readers.fetch_sub(1, std::memory_order_release); return result;
        }
        frame.readers.fetch_sub(1, std::memory_order_release);
    }
    ++pluginInterfacesRejected; return false;
}
}
bool KHZ_PluginUdpReceive(netadr_t* source, unsigned char* data, int length) {
    if (!Sys_IsMainThread() || !source || !data || length < 0 || length > 65507 || pluginUdpDepth >= 4) { ++pluginUdpInvalid; return false; }
    if (!initialized || shuttingDown || !registry.Loaded()) return true;
    khz::plugin_net::Address address{};
    if (!publicPluginAddress(*source, &address)) { ++pluginUdpInvalid; return false; }
    PluginUdpDepth depth; qboolean veto = qfalse;
    KHZ_PluginEvent(KHZPluginEvent::UdpNetEvent, reinterpret_cast<std::uintptr_t>(&address),
        reinterpret_cast<std::uintptr_t>(data), std::uintptr_t(length), reinterpret_cast<std::uintptr_t>(&veto));
    ++pluginUdpReceived;
    if (veto) { ++pluginUdpReceiveVeto; return false; }
    netadr_t native{};
    if (!pluginNativeDatagramAddress(address, &native, true)) { ++pluginUdpInvalid; return false; }
    *source = native; return true;
}
int KHZ_PluginUdpSend(netadr_t* destination, unsigned char* data, int length) {
    if (!Sys_IsMainThread()) { ++pluginUdpInvalid; return -1; }
    if (!destination || !data || length < 0 || length > 0x20000 || pluginUdpDepth >= 4) { ++pluginUdpInvalid; return -1; }
    if (!initialized || shuttingDown || !registry.Loaded()) return 0;
    khz::plugin_net::Address address{};
    if (!publicPluginAddress(*destination, &address)) { ++pluginUdpInvalid; return -1; }
    PluginUdpDepth depth; qboolean veto = qfalse;
    KHZ_PluginEvent(KHZPluginEvent::UdpNetSend, reinterpret_cast<std::uintptr_t>(&address),
        reinterpret_cast<std::uintptr_t>(data), std::uintptr_t(length), reinterpret_cast<std::uintptr_t>(&veto));
    if (veto) { ++pluginUdpSendVeto; return 1; }
    netadr_t native{};
    if (!pluginNativeDatagramAddress(address, &native, false)) { ++pluginUdpInvalid; return -1; }
    *destination = native; return 0;
}
void KHZ_PluginRefreshNetworkInterfaces() {
    owner(); if (com_inServerFrame || authoritativeCallback) { ++apiRejected; return; }
    const unsigned current = publishedPluginInterfaces.load(std::memory_order_acquire);
    for (unsigned offset = 1; offset != pluginInterfaceCatalogs.size(); ++offset) {
        const unsigned next = (current + offset) % pluginInterfaceCatalogs.size(); auto& frame = pluginInterfaceCatalogs[next];
        if (frame.readers.load(std::memory_order_acquire)) continue;
        unsigned count = 0;
        if (!khz::plugin_net::CaptureInterfaces(frame.values.data(), unsigned(frame.values.size()), &count)) { ++pluginInterfacesRejected; return; }
        frame.count = count; publishedPluginInterfaces.store(next, std::memory_order_release); return;
    }
    ++pluginInterfacesRejected;
}
KHZ_API qboolean __cdecl Plugin_IsLANAddress(khz_public_netadr_t* address) { return pluginAddressLAN(address) ? qtrue : qfalse; }
KHZ_API qboolean __cdecl Plugin_NET_CompareAdr(khz_public_netadr_t* a, khz_public_netadr_t* b) { return khz::plugin_net::Compare(a, b) ? qtrue : qfalse; }
KHZ_API qboolean __cdecl Plugin_NET_CompareBaseAdr(khz_public_netadr_t* a, khz_public_netadr_t* b) { return khz::plugin_net::CompareBase(a, b) ? qtrue : qfalse; }
KHZ_API qboolean __cdecl Plugin_NET_CompareBaseAdrMask(khz_public_netadr_t* a, khz_public_netadr_t* b, int bits) { return khz::plugin_net::CompareBase(a, b, bits) ? qtrue : qfalse; }
KHZ_API const char* __cdecl Plugin_NET_AdrToString(khz_public_netadr_t* address) {
    thread_local char text[khz::plugin_net::AddressStringMinimum];
    return khz::plugin_net::FormatAddress(address, khz::plugin_net::Format::Full, text, sizeof(text));
}
KHZ_API const char* __cdecl Plugin_NET_AdrToStringShort(khz_public_netadr_t* address) {
    thread_local char text[khz::plugin_net::AddressStringMinimum];
    return khz::plugin_net::FormatAddress(address, khz::plugin_net::Format::Short, text, sizeof(text));
}
KHZ_API const char* __cdecl Plugin_NET_AdrToStringMT(khz_public_netadr_t* address, char* output, int capacity) {
    return khz::plugin_net::FormatAddress(address, khz::plugin_net::Format::Full, output, capacity);
}
KHZ_API const char* __cdecl Plugin_NET_AdrToStringShortMT(khz_public_netadr_t* address, char* output, int capacity) {
    return khz::plugin_net::FormatAddress(address, khz::plugin_net::Format::Short, output, capacity);
}
KHZ_API const char* __cdecl Plugin_NET_AdrMaskToStringMT(khz_public_netadr_t* address, char* output, int capacity) {
    return khz::plugin_net::FormatAddress(address, khz::plugin_net::Format::Mask, output, capacity);
}
KHZ_API int __cdecl Plugin_NET_StringToAdr(const char* text, khz_public_netadr_t* output, khz_public_netadrtype_t family) {
    if (!output) return 0; *output = {};
    khz::plugin_net::ParsedEndpoint candidate{};
    const auto result = khz::plugin_net::ParseNumeric(text, family, &candidate);
    if (result == khz::plugin_net::NumericResult::NeedsDNS) {
        // Synchronous public return means actual DNS completion. Blocking the
        // native owner is forbidden; rebuilt callers use pinned jobs/threads.
        int plugin; std::uint64_t generation; khz::plugin_net::Address resolved{};
        if (Sys_IsMainThread()) { ++pluginNumericDNSPending; return 0; }
        if (!pluginNetworkContext(_ReturnAddress(), &plugin, &generation) ||
            !khz::plugin_dns::Resolve(unsigned(plugin), generation, candidate, &resolved) ||
            pluginNetworkGenerations[plugin].load(std::memory_order_acquire) != generation) return 0;
        candidate.address = resolved;
    } else if (result != khz::plugin_net::NumericResult::Numeric) return 0;
    candidate.address.sock = candidate.address.type == KHZ_NET_NA_IP ? KHZ_NativeUdpSocket() : 0;
    *output = candidate.address; return candidate.returnCode;
}
KHZ_API qboolean __cdecl Plugin_UdpSendData(khz_public_netadr_t* destination, void* data, int length) {
    int plugin; std::uint64_t generation; netadr_t native{};
    if (!pluginNetworkContext(_ReturnAddress(), &plugin, &generation) || !destination || !data || length < 0 || length > 65507 ||
        (destination->type != KHZ_NET_NA_IP && destination->type != KHZ_NET_NA_BROADCAST) ||
        !pluginNativeDatagramAddress(*destination, &native, false)) { ++pluginUdpSendFailed; return qfalse; }
    // Matches the pinned direct system-send path: no extra OnUdpNetSend event.
    int socket = 0; const bool result = KHZ_NativeUdpSend(native, data, length, &socket);
    destination->sock = socket;
    if (result) ++pluginUdpSent; else ++pluginUdpSendFailed;
    return result ? qtrue : qfalse;
}
KHZ_API void __cdecl Plugin_ServerPacketEvent(khz_public_netadr_t* source, void* data, int length) {
    int plugin; std::uint64_t generation; PluginPacketMailbox::Token token{}; netadr_t native{};
    if (!pluginNetworkContext(_ReturnAddress(), &plugin, &generation) || !source || !data || length < 0 || length > 65507 ||
        !pluginNativeDatagramAddress(*source, &native, true) || !pluginPacketToken(plugin, generation, &token) ||
        !pluginPackets.Push(token, *source, data, length)) { ++pluginInjectionRejected; return; }
    // Even owner/tick callers defer to the next outer service. The host owns a
    // copy and rechecks lifecycle; no connect/map handler runs inside Pmove.
    ++pluginInjected;
}
