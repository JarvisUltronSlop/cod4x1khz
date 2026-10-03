#pragma once
#include <snapshot_header_history.hpp>
#include "core.hpp"

// Decoder-owner or explicitly quiescent access only. A header is recorded only
// after the corresponding native snapshot has decoded and committed.
void KHZ_ResetCommittedSnapshotHeaders(std::uint64_t session);
bool KHZ_RecordCommittedSnapshotHeader(int messageNumber, const khz::WireHeader& header);
bool KHZ_CommittedSnapshotHeader(int messageNumber, khz::WireHeader* output);
// The caller owns the acknowledged simulation barrier and the same map/session.
void KHZ_CopyCommittedSnapshotHeaders(khz::SnapshotHeaderHistory* output);
void KHZ_RestoreCommittedSnapshotHeaders(const khz::SnapshotHeaderHistory& image);
