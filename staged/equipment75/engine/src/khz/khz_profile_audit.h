#pragma once
#include <cstdint>
// Include proposal fragment inside profile.cpp's anonymous namespace before
// CommitUnlocks. Its capture call is after every successful persisted write and
// live/entitlement commit. No capture of an in-progress or failed transaction.
const char* KHZ_ProfileUnlockAuditJSON();
const char* KHZ_ProfileUnlockAuditExportJSON(std::uint64_t expectedSerial);
const char* KHZ_ProfileSnapshotExportJSON();
