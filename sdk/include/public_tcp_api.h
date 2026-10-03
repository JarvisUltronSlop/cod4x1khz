/* Rebuilt public numbered TCP APIs. Four private connections per plugin.
 * Connect must run in a pinned host job/thread; success means connected.
 * GetData/SendData are nonblocking, return actual partial bytes (<=64 KiB),
 * 0 for busy/would-block and -1 for closed/invalid. MT IDs must match the
 * calling pinned module; they cannot impersonate a different plugin.
 */
#ifndef COD4X1KHZ_PUBLIC_TCP_API_H
#define COD4X1KHZ_PUBLIC_TCP_API_H
PAPI qboolean __cdecl Plugin_TcpConnect(int connection, const char* remote);
PAPI qboolean __cdecl Plugin_TcpConnectMT(int pluginID, int connection, const char* remote);
PAPI int __cdecl Plugin_TcpGetData(int connection, void* buffer, int capacity);
PAPI int __cdecl Plugin_TcpGetDataMT(int pluginID, int connection, void* buffer, int capacity);
PAPI int __cdecl Plugin_TcpSendData(int connection, void* data, int length);
PAPI int __cdecl Plugin_TcpSendDataMT(int pluginID, int connection, void* data, int length);
PAPI void __cdecl Plugin_TcpCloseConnection(int connection);
PAPI void __cdecl Plugin_TcpCloseConnectionMT(int pluginID, int connection);
#endif
