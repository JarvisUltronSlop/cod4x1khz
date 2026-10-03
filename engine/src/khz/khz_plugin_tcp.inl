// Included by khz_plugins.cpp after its pinned TLS/HTTP ownership helpers.
namespace {
bool pluginTcpContext(void* caller, int claimedOwner, bool connecting,
    int* plugin, std::uint64_t* generation) {
    if (connecting && (Sys_IsMainThread() || shuttingDown)) { ++apiRejected; return false; }
    if (!pluginHttpContext(caller, !connecting, plugin, generation)) return false;
    if (claimedOwner != -1 && claimedOwner != *plugin) { ++apiRejected; return false; }
    return true;
}
}
KHZ_API qboolean __cdecl Plugin_TcpConnect(int connection, const char* remote) try {
    int plugin; std::uint64_t generation;
    return pluginTcpContext(_ReturnAddress(), -1, true, &plugin, &generation) &&
        khz::plugin_tcp::Connect(plugin, generation, connection, remote) ? qtrue : qfalse;
} catch (...) { ++apiRejected; return qfalse; }
KHZ_API qboolean __cdecl Plugin_TcpConnectMT(int claimed, int connection, const char* remote) try {
    int plugin; std::uint64_t generation;
    if (claimed < 0 || !pluginTcpContext(_ReturnAddress(), claimed, true, &plugin, &generation)) return qfalse;
    return khz::plugin_tcp::Connect(plugin, generation, connection, remote) ? qtrue : qfalse;
} catch (...) { ++apiRejected; return qfalse; }
KHZ_API int __cdecl Plugin_TcpGetData(int connection, void* buffer, int capacity) try {
    int plugin; std::uint64_t generation;
    return pluginTcpContext(_ReturnAddress(), -1, false, &plugin, &generation) ?
        khz::plugin_tcp::Receive(plugin, generation, connection, buffer, capacity) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_TcpGetDataMT(int claimed, int connection, void* buffer, int capacity) try {
    int plugin; std::uint64_t generation;
    if (claimed < 0 || !pluginTcpContext(_ReturnAddress(), claimed, false, &plugin, &generation)) return -1;
    return khz::plugin_tcp::Receive(plugin, generation, connection, buffer, capacity);
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_TcpSendData(int connection, void* data, int length) try {
    int plugin; std::uint64_t generation;
    return pluginTcpContext(_ReturnAddress(), -1, false, &plugin, &generation) ?
        khz::plugin_tcp::Send(plugin, generation, connection, data, length) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_TcpSendDataMT(int claimed, int connection, void* data, int length) try {
    int plugin; std::uint64_t generation;
    if (claimed < 0 || !pluginTcpContext(_ReturnAddress(), claimed, false, &plugin, &generation)) return -1;
    return khz::plugin_tcp::Send(plugin, generation, connection, data, length);
} catch (...) { ++apiRejected; return -1; }
KHZ_API void __cdecl Plugin_TcpCloseConnection(int connection) try {
    int plugin; std::uint64_t generation;
    if (pluginTcpContext(_ReturnAddress(), -1, false, &plugin, &generation)) khz::plugin_tcp::Close(plugin, generation, connection);
} catch (...) { ++apiRejected; }
KHZ_API void __cdecl Plugin_TcpCloseConnectionMT(int claimed, int connection) try {
    int plugin; std::uint64_t generation;
    if (claimed >= 0 && pluginTcpContext(_ReturnAddress(), claimed, false, &plugin, &generation)) khz::plugin_tcp::Close(plugin, generation, connection);
} catch (...) { ++apiRejected; }
