#pragma once

// All three actual source owners must register; no socket or external peer is
// inferred from a copied snapshot. These remain staged until native integration.
void KHZ_ServerNetworkRegisterCheckpoint();
void KHZ_ServerSnapshotRegisterCheckpoint(); // END sv_snapshot_mp.cpp
void KHZ_ServerTransportRegisterCheckpoint(); // END net_chan_mp.cpp
struct KHZ_ArchiveSidecarImage;
// Cross-check against the actual source-owned captured svs, before any provider commits.
bool KHZ_ServerNetworkCapturedArchiveMatches(const KHZ_ArchiveSidecarImage& image);
