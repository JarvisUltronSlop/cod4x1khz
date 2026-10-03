// Include at END of qcommon/net_chan_mp.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_network_checkpoint.h"
#include "khz_server_checkpoint_native.h"
namespace {
using namespace khz::server_native_checkpoint;
struct ServerTransportImage {
    KHZ_ServerCheckpointFrame frame;
    Bytes<sizeof(loopbacks)> loops;
    Bytes<sizeof(laggedPackets)> lagged;
    Bytes<sizeof(tempNetchanPacketBuf)> temporary;
    Bytes<sizeof(s_clientSnapshotData)> snapshotData;
    int profiling, qport;
    bool fakeInitialized, captured;
};
ServerTransportImage serverTransportImage;
bool serverTransportBoundary(char* text, unsigned capacity) {
    if (!context(text, capacity)) return false;
    if (!NET_CheckpointQueuesQuiescent(NS_SERVER) || !NET_CheckpointQueuesQuiescent(NS_CLIENT1))
        return reject(text, capacity, "transport_checkpoint_requires_both_actual_native_queues_drained");
    for (const auto& p : laggedPackets)
        if (p.data || p.length || p.msg.data || p.msg.splitData)
            return reject(text, capacity, "transport_checkpoint_fakelag_allocation_is_not_restored");
    return true;
}
bool serverTransportCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverTransportBoundary(text, capacity)) return false;
    auto& image = serverTransportImage; image.frame = f;
    image.loops.capture(loopbacks); image.lagged.capture(laggedPackets);
    image.temporary.capture(tempNetchanPacketBuf); image.snapshotData.capture(s_clientSnapshotData);
    image.profiling = net_iProfilingOn; image.qport = g_qport; image.fakeInitialized = fakelagInitialized;
    image.captured = true; return true;
}
bool serverTransportValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverTransportBoundary(text, capacity)) return false;
    return serverTransportImage.captured && khz::server_checkpoint::sameLifetime(f, serverTransportImage.frame) ? true :
        reject(text, capacity, "transport_checkpoint_original_owner_or_map_lifetime_changed");
}
void serverTransportRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext()); const auto& image = serverTransportImage;
    image.loops.restore(loopbacks); image.lagged.restore(laggedPackets);
    image.temporary.restore(tempNetchanPacketBuf); image.snapshotData.restore(s_clientSnapshotData);
    net_iProfilingOn = image.profiling; g_qport = image.qport; fakelagInitialized = image.fakeInitialized;
}
void serverTransportDiscard() { serverTransportImage.captured = false; }
}
void KHZ_ServerTransportRegisterCheckpoint() {
    if (!KHZ_ServerCheckpointRequireParts(KHZ_ServerCheckpointDomain::Network, 7))
        Com_Error(ERR_FATAL, "Native transport checkpoint domain could not register");
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Network, 2, sizeof(serverTransportImage),
        serverTransportCapture, serverTransportValidate, serverTransportRestore, serverTransportDiscard};
    if (!KHZ_ServerCheckpointRegister(p)) Com_Error(ERR_FATAL, "Duplicate native transport checkpoint provider");
}
