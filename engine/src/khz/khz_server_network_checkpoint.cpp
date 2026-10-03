#include <universal/q_shared.h>
#include "khz_server_network_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include "khz_archive.h"
#include <server_mp/server_mp.h>
#include <game_mp/g_public_mp.h>
#include <Windows.h>
#include <cstring>

namespace {
using namespace khz::server_native_checkpoint;
struct NetworkImage {
    KHZ_ServerCheckpointFrame frame;
    // Allocate only for an explicit, paused benchmark capture. The complete
    // native archive is large; ordinary matches carry no second copy of it.
    void* statics = nullptr;
    Bytes<sizeof(sv)> server;
    bool captured = false;
};
NetworkImage networkImage;
bool networkBoundary(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!context(reason, capacity)) return false;
    if (f.serverAddress != reinterpret_cast<std::uintptr_t>(&sv) ||
        f.clientsAddress != reinterpret_cast<std::uintptr_t>(svs.clients) ||
        f.entitiesAddress != reinterpret_cast<std::uintptr_t>(g_entities) ||
        f.nativeTime != svs.time || sv.state != SS_GAME || sv.inFrame || sv.restarting ||
        sv.killServer || sv.killReason || svsHeaderValid || !svs.initialized ||
        !sv_maxclients || sv_maxclients->current.integer < 1 || sv_maxclients->current.integer > 16 ||
        sv.gentities != g_entities || sv.gameClients != &level.clients[0].ps ||
        !NET_CheckpointQueuesQuiescent(NS_SERVER))
        return reject(reason, capacity, "network_checkpoint_requires_completed_original_native_server");
    for (unsigned i = 0; i != 64; ++i) {
        const auto& client = svs.clients[i]; const auto& channel = client.header.netchan;
        if (client.dropReason || client.downloading || client.download || client.downloadingWWW ||
            client.clientDownloadingWWW || client.voicePacketCount || channel.unsentFragments || channel.fragmentLength)
            return reject(reason, capacity, "network_checkpoint_external_download_voice_or_fragment_is_active");
        for (const auto* block : client.downloadBlocks)
            if (block) return reject(reason, capacity, "network_checkpoint_download_allocation_is_not_restored");
        if (client.gentity && client.gentity != &g_entities[i])
            return reject(reason, capacity, "network_checkpoint_client_entity_binding_is_foreign");
        if (client.header.state >= CS_CONNECTED && !client.bIsTestClient &&
            (channel.fragmentBuffer != reinterpret_cast<const unsigned char*>(client.netchanIncomingBuffer) ||
             channel.fragmentBufferSize != sizeof(client.netchanIncomingBuffer) ||
             channel.unsentBuffer != reinterpret_cast<const unsigned char*>(client.netchanOutgoingBuffer) ||
             channel.unsentBufferSize != sizeof(client.netchanOutgoingBuffer)))
            return reject(reason, capacity, "network_checkpoint_netchan_buffer_is_not_original_client_storage");
    }
    return true;
}
bool networkCapture(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!networkBoundary(f, reason, capacity)) return false;
    auto& image = networkImage;
    if (image.statics) return reject(reason, capacity, "network_checkpoint_image_already_allocated");
    image.statics = VirtualAlloc(nullptr, sizeof(svs), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image.statics) return reject(reason, capacity, "network_checkpoint_native_archive_image_allocation_failed");
    image.frame = f; image.server.capture(&sv);
    std::memcpy(image.statics, &svs, sizeof(svs)); image.captured = true; return true;
}
bool networkValidate(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!networkBoundary(f, reason, capacity)) return false;
    const auto& image = networkImage;
    if (!image.captured || !image.statics || !khz::server_checkpoint::sameLifetime(f, image.frame))
        return reject(reason, capacity, "network_checkpoint_original_map_or_native_storage_changed");
    const auto& saved = *static_cast<const serverStatic_t*>(image.statics);
    for (unsigned i = 0; i != 64; ++i) {
        const auto& a = saved.clients[i]; const auto& b = svs.clients[i];
        if (a.header.state != b.header.state || a.bIsTestClient != b.bIsTestClient ||
            a.gentity != b.gentity || a.scriptId != b.scriptId || a.serverId != b.serverId ||
            a.challenge != b.challenge || a.lastConnectTime != b.lastConnectTime ||
            a.header.netchan.qport != b.header.netchan.qport ||
            std::memcmp(&a.header.netchan.remoteAddress, &b.header.netchan.remoteAddress, sizeof(netadr_t)) ||
            a.header.netchan.fragmentBuffer != b.header.netchan.fragmentBuffer ||
            a.header.netchan.unsentBuffer != b.header.netchan.unsentBuffer ||
            std::memcmp(a.cdkeyHash, b.cdkeyHash, sizeof(a.cdkeyHash)))
            return reject(reason, capacity, "network_checkpoint_admission_slot_generation_or_endpoint_changed");
    }
    // Native models belong to the same pinned BSP/hunk lifetime. The World,
    // SL and collision providers restore their distinct mutable storage.
    const auto& savedServer = *reinterpret_cast<const server_t*>(image.server.value);
    if (std::memcmp(savedServer.models, sv.models, sizeof(sv.models)) ||
        savedServer.gentities != sv.gentities || savedServer.gameClients != sv.gameClients ||
        savedServer.gentitySize != sv.gentitySize || savedServer.gameClientSize != sv.gameClientSize)
        return reject(reason, capacity, "network_checkpoint_loaded_native_model_or_game_arena_changed");
    return true;
}
void networkRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext() && networkImage.captured && networkImage.statics);
    std::memcpy(&svs, networkImage.statics, sizeof(svs)); networkImage.server.restore(&sv);
}
void networkDiscard() {
    networkImage.captured = false;
    if (networkImage.statics) { VirtualFree(networkImage.statics, 0, MEM_RELEASE); networkImage.statics = nullptr; }
}
}
bool KHZ_ServerNetworkCapturedArchiveMatches(const KHZ_ArchiveSidecarImage& image) {
    return Sys_IsMainThread() && KHZ_ServerCheckpointContext() && networkImage.captured &&
        networkImage.statics && KHZ_ArchiveSidecarMatchesNativeImage(image,
            *static_cast<const serverStatic_t*>(networkImage.statics));
}
void KHZ_ServerNetworkRegisterCheckpoint() {
    if (!KHZ_ServerCheckpointRequireParts(KHZ_ServerCheckpointDomain::Network, 7) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Network, &sv, sizeof(sv)) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Network, &svs, sizeof(svs)))
        Com_Error(ERR_FATAL, "Native server network checkpoint ranges could not register");
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Network, 0, sizeof(svs) + sizeof(networkImage),
        networkCapture, networkValidate, networkRestore, networkDiscard};
    if (!KHZ_ServerCheckpointRegister(p)) Com_Error(ERR_FATAL, "Duplicate native server network checkpoint provider");
}
