// Include at END of server_mp/sv_snapshot_mp.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_network_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include "khz_archive.h"
namespace {
using namespace khz::server_native_checkpoint;
struct ServerSnapshotImage {
    KHZ_ServerCheckpointFrame frame;
    Bytes<sizeof(svsHeader)> header;
    Bytes<sizeof(g_archiveMsg)> archiveMessage;
    Bytes<sizeof(tempServerMsgBuf)> temporary;
    KHZ_ArchiveSidecarImage archive;
    int valid;
    bool captured;
};
ServerSnapshotImage serverSnapshotImage;
bool serverSnapshotBoundary(char* text, unsigned capacity) {
    if (!context(text, capacity)) return false;
    if (svsHeaderValid || g_archivingSnapshot || sv.inFrame || sv.state != SS_GAME)
        return reject(text, capacity, "snapshot_checkpoint_native_snapshot_dispatch_is_active");
    if (g_archiveMsg.data && !inside(g_archiveMsg.data, svs.archivedSnapshotBuffer, sizeof(svs.archivedSnapshotBuffer)) &&
        !inside(g_archiveMsg.data, tempServerMsgBuf, sizeof(tempServerMsgBuf)))
        return reject(text, capacity, "snapshot_checkpoint_archive_message_has_foreign_backing_storage");
    if (g_archiveMsg.splitData)
        return reject(text, capacity, "snapshot_checkpoint_split_archive_message_is_unsupported");
    return true;
}
bool serverSnapshotCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverSnapshotBoundary(text, capacity)) return false;
    auto& image = serverSnapshotImage; image.frame = f; image.header.capture(&svsHeader);
    image.archiveMessage.capture(&g_archiveMsg); image.temporary.capture(tempServerMsgBuf);
    if (!KHZ_ArchiveCopySidecar(&image.archive))
        return reject(text, capacity, "snapshot_checkpoint_archive_owner_is_not_completed");
    image.valid = svsHeaderValid; image.captured = true; return true;
}
bool serverSnapshotValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverSnapshotBoundary(text, capacity)) return false;
    return serverSnapshotImage.captured && khz::server_checkpoint::sameLifetime(f, serverSnapshotImage.frame) &&
        KHZ_ArchiveValidateSidecar(serverSnapshotImage.archive, serverSnapshotImage.frame.nativeTime) &&
        KHZ_ServerNetworkCapturedArchiveMatches(serverSnapshotImage.archive) ? true :
        reject(text, capacity, "snapshot_checkpoint_original_owner_or_map_lifetime_changed");
}
void serverSnapshotRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext()); const auto& image = serverSnapshotImage;
    image.header.restore(&svsHeader); image.archiveMessage.restore(&g_archiveMsg);
    image.temporary.restore(tempServerMsgBuf); svsHeaderValid = image.valid;
    KHZ_ArchiveRestoreSidecar(image.archive);
}
void serverSnapshotDiscard() { serverSnapshotImage.captured = false; }
}
void KHZ_ServerSnapshotRegisterCheckpoint() {
    if (!KHZ_ServerCheckpointRequireParts(KHZ_ServerCheckpointDomain::Network, 7))
        Com_Error(ERR_FATAL, "Native snapshot checkpoint domain could not register");
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Network, 1, sizeof(serverSnapshotImage),
        serverSnapshotCapture, serverSnapshotValidate, serverSnapshotRestore, serverSnapshotDiscard};
    if (!KHZ_ServerCheckpointRegister(p)) Com_Error(ERR_FATAL, "Duplicate native snapshot checkpoint provider");
}
