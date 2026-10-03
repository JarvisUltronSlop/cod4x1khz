#include "khz_snapshot_headers.h"
#include <snapshot_header_history.hpp>

namespace { khz::SnapshotHeaderHistory committedHeaders; }
void KHZ_ResetCommittedSnapshotHeaders(std::uint64_t session) { committedHeaders.reset(session); }
bool KHZ_RecordCommittedSnapshotHeader(int messageNumber, const khz::WireHeader& header) { return committedHeaders.commit(messageNumber, header); }
bool KHZ_CommittedSnapshotHeader(int messageNumber, khz::WireHeader* output) { return committedHeaders.read(messageNumber, output); }
void KHZ_CopyCommittedSnapshotHeaders(khz::SnapshotHeaderHistory* output) { if (output) *output = committedHeaders; }
void KHZ_RestoreCommittedSnapshotHeaders(const khz::SnapshotHeaderHistory& image) { committedHeaders = image; }
