/* Actual SDK DLL fixture. A controlled echo listener is supplied by the
 * native harness. Connect/DNS runs only in a pinned job; completion is owner.
 */
static volatile LONG tcp_fixture_active, tcp_fixture_finished;
static char tcp_fixture_remote[256], tcp_fixture_remote6[256];
static unsigned tcp_fixture_cases, tcp_fixture_failures;
static int tcp_fixture_owner, tcp_fixture_completions, tcp_fixture_main_rejected, tcp_fixture_tick_rejected;
static unsigned char tcp_fixture_sent[98317], tcp_fixture_received[98317];
static void tcp_check(int passed) { ++tcp_fixture_cases; if (!passed) ++tcp_fixture_failures; }
static int tcp_fixture_connect(int owner, int connection, const char* remote, int mt) {
    return mt ? Plugin_TcpConnectMT(owner, connection, remote) : Plugin_TcpConnect(connection, remote);
}
static int tcp_fixture_echo(int owner, int connection, int mt) {
    unsigned sent = 0, received = 0; int bounded = 1; ULONGLONG end = GetTickCount64() + 2000;
    memset(tcp_fixture_received, 0, sizeof(tcp_fixture_received));
    while ((sent < sizeof(tcp_fixture_sent) || received < sizeof(tcp_fixture_received)) && GetTickCount64() < end) {
        int count;
        if (sent < sizeof(tcp_fixture_sent)) {
            count = mt ? Plugin_TcpSendDataMT(owner, connection, tcp_fixture_sent + sent, (int)(sizeof(tcp_fixture_sent) - sent)) :
                Plugin_TcpSendData(connection, tcp_fixture_sent + sent, (int)(sizeof(tcp_fixture_sent) - sent));
            if (count < 0) return 0;
            if (count > 65536 || (unsigned)count > sizeof(tcp_fixture_sent) - sent) return 0;
            sent += (unsigned)count;
        }
        if (received < sizeof(tcp_fixture_received)) {
            count = mt ? Plugin_TcpGetDataMT(owner, connection, tcp_fixture_received + received, (int)(sizeof(tcp_fixture_received) - received)) :
                Plugin_TcpGetData(connection, tcp_fixture_received + received, (int)(sizeof(tcp_fixture_received) - received));
            if (count < 0) return 0;
            if (count > 65536 || (unsigned)count > sizeof(tcp_fixture_received) - received) return 0;
            received += (unsigned)count;
        }
        Sleep(1);
    }
    return bounded && sent == sizeof(tcp_fixture_sent) && received == sizeof(tcp_fixture_received) &&
        !memcmp(tcp_fixture_sent, tcp_fixture_received, sizeof(tcp_fixture_sent));
}
static int tcp_fixture_reconnect(int owner, int slot, const char* remote, int mt) {
    ULONGLONG end = GetTickCount64() + 1000;
    while (GetTickCount64() < end) {
        if (tcp_fixture_connect(owner, slot, remote, mt)) return 1;
        Sleep(1);
    }
    return 0;
}
static void tcp_fixture_work(void* unused) {
    unsigned i; unsigned char byte = 0; int owner = Plugin_GetPluginID();
    for (i = 0; i < sizeof(tcp_fixture_sent); ++i) tcp_fixture_sent[i] = (unsigned char)(i * 17 + 3);
    tcp_check(!Plugin_IsMainThread() && owner >= 0);
    tcp_check(!Plugin_TcpConnect(-1, tcp_fixture_remote) && !Plugin_TcpConnect(4, tcp_fixture_remote));
    tcp_check(!Plugin_TcpConnectMT(owner + 1, 0, tcp_fixture_remote));
    tcp_check(Plugin_TcpConnect(0, tcp_fixture_remote));
    tcp_check(Plugin_TcpGetData(0, &byte, 1) == 0);
    tcp_check(!Plugin_TcpConnect(0, tcp_fixture_remote));
    tcp_check(Plugin_TcpGetDataMT(owner + 1, 0, &byte, 1) == -1 && Plugin_TcpSendDataMT(owner + 1, 0, &byte, 1) == -1);
    Plugin_TcpCloseConnectionMT(owner + 1, 0);
    tcp_check(tcp_fixture_echo(owner, 0, 0));
    Plugin_TcpCloseConnection(0);
    tcp_check(tcp_fixture_reconnect(owner, 0, tcp_fixture_remote, 1));
    tcp_check(tcp_fixture_echo(owner, 0, 1));
    Plugin_TcpCloseConnectionMT(owner, 0);
    for (i = 0; i < 4; ++i) tcp_check(tcp_fixture_reconnect(owner, (int)i, tcp_fixture_remote, 1));
    tcp_check(!Plugin_TcpConnectMT(owner, 4, tcp_fixture_remote));
    for (i = 0; i < 4; ++i) tcp_check(tcp_fixture_echo(owner, (int)i, 1));
    for (i = 0; i < 4; ++i) Plugin_TcpCloseConnectionMT(owner, (int)i);
    tcp_check(tcp_fixture_reconnect(owner, 0, tcp_fixture_remote, 0));
    { char close[] = "CLOSE\n"; int status = 0; ULONGLONG end = GetTickCount64() + 2000;
      tcp_check(Plugin_TcpSendData(0, close, 6) == 6);
      while (GetTickCount64() < end) { status = Plugin_TcpGetData(0, &byte, 1); if (status < 0) break; Sleep(1); }
      tcp_check(status == -1); Plugin_TcpCloseConnection(0); }
    if (tcp_fixture_remote6[0]) {
        tcp_check(tcp_fixture_reconnect(owner, 0, tcp_fixture_remote6, 1));
        tcp_check(tcp_fixture_echo(owner, 0, 1)); Plugin_TcpCloseConnectionMT(owner, 0);
    }
    InterlockedExchange(&tcp_fixture_finished, 1);
}
static void tcp_fixture_completion(void* unused) {
    tcp_fixture_owner = Plugin_IsMainThread() && !Plugin_IsAuthoritativeTick(); ++tcp_fixture_completions;
    InterlockedExchange(&tcp_fixture_active, 0);
}
static void tcp_fixture_start(void) {
    const char* remote; const char* remote6;
    if (Plugin_Cmd_Argc() < 2 || Plugin_Cmd_Argc() > 3) { Plugin_Printf("Usage: khz_fixture_tcp_start <echo remote> [IPv6 echo remote]\n"); return; }
    remote = Plugin_Cmd_Argv(1); remote6 = Plugin_Cmd_Argc() == 3 ? Plugin_Cmd_Argv(2) : "";
    if (!*remote || strlen(remote) >= sizeof(tcp_fixture_remote) || strlen(remote6) >= sizeof(tcp_fixture_remote6)) return;
    if (InterlockedCompareExchange(&tcp_fixture_active, 1, 0)) return;
    strcpy(tcp_fixture_remote, remote); strcpy(tcp_fixture_remote6, remote6);
    tcp_fixture_cases = tcp_fixture_failures = 0; tcp_fixture_owner = 0;
    InterlockedExchange(&tcp_fixture_finished, 0);
    tcp_fixture_main_rejected = !Plugin_TcpConnect(0, remote) && !Plugin_TcpConnectMT(Plugin_GetPluginID(), 0, remote);
    if (!Plugin_QueueJob(tcp_fixture_work, NULL, tcp_fixture_completion)) InterlockedExchange(&tcp_fixture_active, 0);
}
static void tcp_fixture_tick_probe(void) {
    tcp_fixture_tick_rejected = !Plugin_TcpConnect(0, "127.0.0.1:1") && Plugin_IsAuthoritativeTick();
}
static void tcp_fixture_report(void) {
    LONG active = InterlockedCompareExchange(&tcp_fixture_active, 0, 0);
    LONG finished = InterlockedCompareExchange(&tcp_fixture_finished, 0, 0);
    if (active) { Plugin_Printf("KHZ_PLUGIN_TCP_FIXTURE active=1\n"); return; }
    Plugin_Printf("KHZ_PLUGIN_TCP_FIXTURE active=%ld finished=%ld cases=%u failures=%u completions=%d owner=%d main_rejected=%d tick_rejected=%d\n",
        active, finished, tcp_fixture_cases, tcp_fixture_failures, tcp_fixture_completions, tcp_fixture_owner,
        tcp_fixture_main_rejected, tcp_fixture_tick_rejected);
}
