#include <universal/q_shared.h>
#include "khz_profile_client.h"
#include "khz_profile.h"
#include "khz_runtime.h"
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <cstdio>

bool KHZ_ProfilePrivateAddress(const netadr_t& address)
{
    if (address.type == NA_LOOPBACK) return true;
    if (address.type != NA_IP) return false;
    return address.ip[0] == 127 || address.ip[0] == 10
        || (address.ip[0] == 172 && address.ip[1] >= 16 && address.ip[1] <= 31)
        || (address.ip[0] == 192 && address.ip[1] == 168);
}

namespace {
bool UnlockConnected()
{
    // A session is set only by the validated custom wire header and cleared by
    // native disconnect. A stock or partially established connection cannot use
    // this path; profile changes and stats transfer must agree on the protocol.
    const auto session = KHZ_AcceptedClientSession();
    const auto* connection = CL_GetLocalClientConnection(0);
    if (CL_GetLocalClientConnectionState(0) != CA_ACTIVE || !session || !connection) {
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Wait for an active cod4x1khz match or return to the main menu.\n");
        return false;
    }
    const auto server = connection->serverAddress;
    if (!KHZ_ProfilePrivateAddress(server) || (server.type == NA_IP && !server.port)) {
        Com_PrintError(CON_CHANNEL_CLIENT, "unlockall: Connected profile refresh is limited to localhost and private LAN servers.\n");
        return false;
    }
    if (KHZ_ProfileHasAllUnlocks()) {
        Com_Printf(CON_CHANNEL_CLIENT, "unlockall: This profile already has the saved stock unlocks.\n");
        return true;
    }
    if (!KHZ_ProfileCommitAllUnlocks()) return false;
    // Match the native reconnect path using a numeric endpoint. In particular,
    // CL_Reconnect_f rejects the name 'localhost'. The reconnect marker invokes
    // native connect without the explicit-disconnect UI callback, preserving an
    // owned private server while its normal stats handshake reloads the profile.
    char command[96];
    if (server.type == NA_LOOPBACK) snprintf(command, sizeof(command), "connect localhost reconnect\n");
    else {
        const unsigned port = (server.port >> 8) | ((server.port & 255u) << 8);
        snprintf(command, sizeof(command), "connect %u.%u.%u.%u:%u reconnect\n",
            server.ip[0], server.ip[1], server.ip[2], server.ip[3], port);
    }
    Cbuf_AddText(0, command);
    Com_Printf(CON_CHANNEL_CLIENT, "Saved unlocks will refresh on reconnect; the private match server stays running.\n");
    return true;
}
}

void KHZ_InitProfileClientFeatures()
{
#ifndef KISAK_DEDICATED
    KHZ_ProfileSetConnectedUnlockHandler(UnlockConnected);
#endif
}
