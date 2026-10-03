/* Pinned CoD4x public address representation; this is NOT Kisak's native
 * 20-byte netadr_t. The host translates fields, never casts between layouts.
 * Define KHZ_PUBLIC_NET_NO_SHORT_NAMES in a native translation unit. */
#ifndef COD4X1KHZ_PUBLIC_NET_ADDRESS_H
#define COD4X1KHZ_PUBLIC_NET_ADDRESS_H
#include <stdint.h>
#include <stddef.h>
typedef enum khz_public_netadrtype_t {
    KHZ_NET_NA_BAD = 0, KHZ_NET_NA_BOT = 0,
    KHZ_NET_NA_LOOPBACK = 2, KHZ_NET_NA_BROADCAST = 3,
    KHZ_NET_NA_IP = 4, KHZ_NET_NA_IP6 = 5,
    KHZ_NET_NA_TCP = 6, KHZ_NET_NA_TCP6 = 7,
    KHZ_NET_NA_MULTICAST6 = 8, KHZ_NET_NA_UNSPEC = 9,
    KHZ_NET_NA_DOWN = 10
} khz_public_netadrtype_t;
typedef struct khz_public_netadr_t {
    khz_public_netadrtype_t type;
    int32_t scope_id;
    uint16_t port; /* Network byte order; format as an unsigned 16-bit port. */
    uint16_t pad;
    int32_t sock;
    union { unsigned char ip[4]; unsigned char ipx[10]; unsigned char ip6[16]; };
} khz_public_netadr_t;
#if defined(__cplusplus)
static_assert(sizeof(khz_public_netadrtype_t) == 4 && sizeof(khz_public_netadr_t) == 32, "public address ABI");
static_assert(offsetof(khz_public_netadr_t, scope_id) == 4 && offsetof(khz_public_netadr_t, port) == 8 &&
    offsetof(khz_public_netadr_t, pad) == 10 && offsetof(khz_public_netadr_t, sock) == 12 &&
    offsetof(khz_public_netadr_t, ip6) == 16, "public address field offsets");
#else
typedef char khz_net_size_assert[(sizeof(khz_public_netadrtype_t) == 4 && sizeof(khz_public_netadr_t) == 32) ? 1 : -1];
typedef char khz_net_offset_assert[(offsetof(khz_public_netadr_t, scope_id) == 4 &&
    offsetof(khz_public_netadr_t, port) == 8 && offsetof(khz_public_netadr_t, pad) == 10 &&
    offsetof(khz_public_netadr_t, sock) == 12 && offsetof(khz_public_netadr_t, ip6) == 16) ? 1 : -1];
#endif
#ifndef KHZ_PUBLIC_NET_NO_SHORT_NAMES
typedef khz_public_netadrtype_t netadrtype_t;
typedef khz_public_netadr_t netadr_t;
#define NA_BAD KHZ_NET_NA_BAD
#define NA_BOT KHZ_NET_NA_BOT
#define NA_LOOPBACK KHZ_NET_NA_LOOPBACK
#define NA_BROADCAST KHZ_NET_NA_BROADCAST
#define NA_IP KHZ_NET_NA_IP
#define NA_IP6 KHZ_NET_NA_IP6
#define NA_TCP KHZ_NET_NA_TCP
#define NA_TCP6 KHZ_NET_NA_TCP6
#define NA_MULTICAST6 KHZ_NET_NA_MULTICAST6
#define NA_UNSPEC KHZ_NET_NA_UNSPEC
#define NA_DOWN KHZ_NET_NA_DOWN
#endif
#endif
