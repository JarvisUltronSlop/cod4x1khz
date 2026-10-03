/* Actual native network fixture. Every target is supplied by an owned
 * loopback harness; no internet host, global ban, or combat state is touched. */
static volatile LONG udp_job_active, udp_job_done, udp_job_good;
static char udp_job_endpoint[256];
static unsigned udp_direct_cases, udp_direct_failures, udp_rx_drop, udp_rx_edit, udp_rx_address;
static unsigned udp_tx_drop, udp_tx_edit, udp_tx_address, udp_direct_events, udp_inject_events;
static unsigned udp_callback_layout, udp_callback_owner;
static void udp_fixture_report(void);
static int udp_find(const void* data, int size, const char* text) {
    const unsigned char* bytes = (const unsigned char*)data;
    unsigned count = (unsigned)strlen(text), i;
    if (!bytes || size < 0 || count > (unsigned)size) return -1;
    for (i = 0; i + count <= (unsigned)size; ++i) if (!memcmp(bytes + i, text, count)) return (int)i;
    return -1;
}
static void udp_check(int condition) { ++udp_direct_cases; if (!condition) ++udp_direct_failures; }
static int udp_endpoint(netadr_t* address) {
    return Plugin_Cmd_Argc() == 3 && Plugin_NET_StringToAdr(Plugin_Cmd_Argv(2), address, NA_IP) == 1 &&
        address->type == NA_IP && address->ip[0] == 127 && address->ip[1] == 0 && address->ip[2] == 0 && address->ip[3] == 1;
}
static void udp_job(void* unused) {
    netadr_t address, missing; unsigned char bytes[] = "KHZ_UDP_WORKER_V1";
    int good = Plugin_NET_StringToAdr(udp_job_endpoint, &address, NA_IP) == 1 && address.type == NA_IP &&
        address.ip[0] == 127 && address.sock != 0 && address.sock != -1;
    good = good && Plugin_UdpSendData(&address, bytes, (int)sizeof(bytes) - 1);
    memset(&missing, 0x5a, sizeof(missing));
    /* Unknown host main rejection is covered separately; native worker checks
     * actual localhost resolution without ever opening a TCP connection. */
    good = good && Plugin_NET_StringToAdr("localhost", &missing, NA_IP) == 2 && missing.type == NA_IP &&
        missing.ip[0] == 127 && net_host_port(missing.port) == 28960 && missing.sock == address.sock;
    good = good && Plugin_NET_StringToAdr("localhost:32768", &missing, NA_TCP) == 1 && missing.type == NA_TCP &&
        missing.ip[0] == 127 && net_host_port(missing.port) == 32768 && missing.sock == 0;
    good = good && Plugin_NET_StringToAdr("localhost:65535", &missing, NA_IP6) == 1 && missing.type == NA_IP6 &&
        net_host_port(missing.port) == 65535 && missing.sock == 0;
    InterlockedExchange(&udp_job_good, good); InterlockedExchange(&udp_job_done, 1);
}
static void udp_job_complete(void* unused) { InterlockedExchange(&udp_job_active, 0); }
static void udp_fixture_command(void) {
    const char* action = Plugin_Cmd_Argv(1); netadr_t address, unsupported;
    unsigned char direct[] = "KHZ_UDP_DIRECT_V1";
    unsigned char injected[] = "\xff\xff\xff\xffgetinfo KHZ_UDP_INJECT_COPY";
    unsigned char badprotocol[] = "\xff\xff\xff\xff" "connect \"\\protocol\\1\\challenge\\0\\qport\\28984\\name\\injected\"";
    unsigned char badproof[] = "\xff\xff\xff\xff" "connect \"\\protocol\\10002\\challenge\\0\\qport\\28984\\name\\injected\"";
    if (!strcmp(action, "report")) { udp_fixture_report(); return; }
    if (!strcmp(action, "drop")) { Plugin_Printf("KHZ_UDP_TX_DROP\n"); return; }
    if (!strcmp(action, "edit")) { Plugin_Printf("KHZ_UDP_TX_EDIT A\n"); return; }
    if (!strcmp(action, "address")) { Plugin_Printf("KHZ_UDP_TX_ADDRESS\n"); return; }
    if (!strcmp(action, "worker") && Plugin_Cmd_Argc() == 3 && !strcmp(Plugin_Cmd_Argv(2), "localhost:28984")) {
        if (InterlockedCompareExchange(&udp_job_active, 1, 0)) return;
        strncpy(udp_job_endpoint, Plugin_Cmd_Argv(2), sizeof(udp_job_endpoint) - 1);
        InterlockedExchange(&udp_job_done, 0); InterlockedExchange(&udp_job_good, 0);
        if (!Plugin_QueueJob(udp_job, NULL, udp_job_complete)) InterlockedExchange(&udp_job_active, 0);
        Plugin_Printf("KHZ_UDP_WORKER queued=%ld\n", InterlockedCompareExchange(&udp_job_active, 0, 0)); return;
    }
    if (!udp_endpoint(&address)) { Plugin_Printf("KHZ_UDP_FIXTURE requires loopback endpoint\n"); return; }
    if (!strcmp(action, "direct")) {
        udp_check(Plugin_UdpSendData(&address, direct, sizeof(direct) - 1));
        udp_check(address.sock != 0 && address.sock != -1);
        udp_check(!Plugin_UdpSendData(NULL, direct, sizeof(direct) - 1));
        udp_check(!Plugin_UdpSendData(&address, NULL, 0));
        udp_check(!Plugin_UdpSendData(&address, direct, -1));
        udp_check(!Plugin_UdpSendData(&address, direct, 65508));
        Plugin_NET_StringToAdr("[::1]:28984", &unsupported, NA_IP6);
        udp_check(!Plugin_UdpSendData(&unsupported, direct, sizeof(direct) - 1));
        Plugin_Printf("KHZ_UDP_DIRECT cases=%u failures=%u socket=%d\n", udp_direct_cases, udp_direct_failures, address.sock);
    } else if (!strcmp(action, "inject")) {
        Plugin_ServerPacketEvent(&address, injected, sizeof(injected) - 1);
        memset(injected, 'X', sizeof(injected)); memset(&address, 0, sizeof(address));
        Plugin_Printf("KHZ_UDP_INJECT queued_copy=1\n");
    } else if (!strcmp(action, "badprotocol")) {
        Plugin_ServerPacketEvent(&address, badprotocol, sizeof(badprotocol) - 1);
        Plugin_Printf("KHZ_UDP_INJECT protocol_rejection_requested=1\n");
    } else if (!strcmp(action, "badproof")) {
        Plugin_ServerPacketEvent(&address, badproof, sizeof(badproof) - 1);
        Plugin_Printf("KHZ_UDP_INJECT proof_rejection_requested=1\n");
    } else if (!strcmp(action, "cancel")) {
        Plugin_ServerPacketEvent(&address, injected, sizeof(injected) - 1);
        Plugin_Printf("KHZ_UDP_INJECT cancellation_requested=1\n");
        Plugin_Error(P_ERROR_DISABLE, "Isolated queued-injection generation cancellation fixture");
    }
}
static void udp_fixture_report(void) {
    netadr_t result; int main_dns;
    memset(&result, 0x5a, sizeof(result)); main_dns = Plugin_NET_StringToAdr("localhost:28984", &result, NA_IP);
    Plugin_Printf("KHZ_UDP_REPORT cases=%u failures=%u rx_drop=%u rx_edit=%u rx_address=%u tx_drop=%u tx_edit=%u tx_address=%u direct_events=%u inject_events=%u callback_layout=%u callback_owner=%u worker_done=%ld worker_good=%ld main_dns=%d main_output=%u\n",
        udp_direct_cases, udp_direct_failures, udp_rx_drop, udp_rx_edit, udp_rx_address, udp_tx_drop, udp_tx_edit,
        udp_tx_address, udp_direct_events, udp_inject_events, udp_callback_layout, udp_callback_owner,
        InterlockedCompareExchange(&udp_job_done, 0, 0), InterlockedCompareExchange(&udp_job_good, 0, 0), main_dns, (unsigned)result.type);
}
PCL void OnUdpNetEvent(netadr_t* from, void* data, int size, qboolean* returnNow) {
    if (udp_find(data, size, "KHZ_UDP_RX_") < 0 && udp_find(data, size, "KHZ_UDP_INJECT_COPY") < 0) return;
    if (from && sizeof(*from) == 32 && from->type == NA_IP && from->ip[0] == 127 && from->sock != 0 && from->sock != -1) ++udp_callback_layout;
    if (Plugin_IsMainThread() && !Plugin_IsAuthoritativeTick()) ++udp_callback_owner;
    if (udp_find(data, size, "KHZ_UDP_INJECT_COPY") >= 0) ++udp_inject_events;
    if (udp_find(data, size, "KHZ_UDP_RX_DROP") >= 0) { ++udp_rx_drop; *returnNow = qtrue; }
    if (udp_find(data, size, "KHZ_UDP_RX_EDIT") >= 0) {
        int index = udp_find(data, size, "EDIT"); memcpy((unsigned char*)data + index, "PASS", 4); ++udp_rx_edit;
    }
    if (udp_find(data, size, "KHZ_UDP_RX_ADDRESS") >= 0 && from) { from->port = (unsigned short)((28985u >> 8) | ((28985u & 255) << 8)); ++udp_rx_address; }
}
PCL void OnUdpNetSend(netadr_t* to, void* data, int length, qboolean* returnNow) {
    int index;
    if (udp_find(data, length, "KHZ_UDP_DIRECT_V1") >= 0 || udp_find(data, length, "KHZ_UDP_WORKER_V1") >= 0) ++udp_direct_events;
    if (udp_find(data, length, "KHZ_UDP_TX_DROP") >= 0) { ++udp_tx_drop; *returnNow = qtrue; }
    index = udp_find(data, length, "KHZ_UDP_TX_EDIT A");
    if (index >= 0) { ((unsigned char*)data)[index + strlen("KHZ_UDP_TX_EDIT ")] = 'B'; ++udp_tx_edit; }
    if (udp_find(data, length, "KHZ_UDP_TX_ADDRESS") >= 0 && to) { to->port = (unsigned short)((28985u >> 8) | ((28985u & 255) << 8)); ++udp_tx_address; }
}
