/* Compiled as C: pinned field spelling, enum constants and anonymous union. */
#include <public_net_address.h>
int KHZ_PublicNetAddressCSmoke(void) {
    netadr_t value = { 0 };
    netadrtype_t type = NA_IP6;
    value.type = type; value.scope_id = 7; value.port = 65535;
    value.pad = 0; value.sock = -1; value.ip6[15] = 1;
    return sizeof(value) == 32 && value.type == NA_IP6 && value.ip[0] == 0 &&
        value.ip6[15] == 1 && value.scope_id == 7 && value.port == 65535 &&
        value.sock == -1 && NA_BAD == NA_BOT && NA_TCP6 == 7;
}
