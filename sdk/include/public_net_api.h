/* Public networking declarations. The host always uses the explicit
 * public address type, even when its translation unit has native netadr_t.
 * Rebuilt plugin code sees netadr_t as an alias of this pinned representation.
 * Hostname resolution belongs to a pinned job/thread; numeric/cached queries
 * and bounded comparisons/formatting may run on the authoritative owner.
 * Hostname resolution requires a pinned plugin job/thread and returns only
 * actual bounded resolver completion. Native owner hostname calls fail0.
 * Direct UDP sends use the configured native IPv4 socket and actual kernel
 * success, bypassing OnUdpNetSend as upstream does. Injection copies bytes
 * into a bounded generation/session queue and dispatches outside gameplay
 * ticks; it bypasses OnUdpNetEvent and retains native protocol validation.
 * Native UDP IPv6 remains unsupported pending real transport migration. */
#ifndef COD4X1KHZ_PUBLIC_NET_API_H
#define COD4X1KHZ_PUBLIC_NET_API_H
#ifdef KHZ_PLUGIN_NATIVE_TYPES
#define KHZ_PUBLIC_NET_NO_SHORT_NAMES
#include "public_net_address.h"
#undef KHZ_PUBLIC_NET_NO_SHORT_NAMES
#else
#include "public_net_address.h"
#endif
PAPI int __cdecl Plugin_NET_StringToAdr(const char* text, khz_public_netadr_t* address, khz_public_netadrtype_t family);
PAPI qboolean __cdecl Plugin_NET_CompareAdr(khz_public_netadr_t* a, khz_public_netadr_t* b);
PAPI qboolean __cdecl Plugin_NET_CompareBaseAdr(khz_public_netadr_t* a, khz_public_netadr_t* b);
PAPI qboolean __cdecl Plugin_NET_CompareBaseAdrMask(khz_public_netadr_t* a, khz_public_netadr_t* b, int mask);
PAPI qboolean __cdecl Plugin_IsLANAddress(khz_public_netadr_t* address);
PAPI const char* __cdecl Plugin_NET_AdrToString(khz_public_netadr_t* address);
PAPI const char* __cdecl Plugin_NET_AdrToStringShort(khz_public_netadr_t* address);
PAPI const char* __cdecl Plugin_NET_AdrToStringMT(khz_public_netadr_t* address, char* buffer, int capacity);
PAPI const char* __cdecl Plugin_NET_AdrToStringShortMT(khz_public_netadr_t* address, char* buffer, int capacity);
PAPI const char* __cdecl Plugin_NET_AdrMaskToStringMT(khz_public_netadr_t* address, char* buffer, int capacity);
PAPI qboolean __cdecl Plugin_UdpSendData(khz_public_netadr_t* destination, void* data, int length);
PAPI void __cdecl Plugin_ServerPacketEvent(khz_public_netadr_t* source, void* data, int length);
#endif
