#pragma once
#include <cstdint>
#include <bgame/bg_local.h>

// Main/actual dedicated owner only. This diagnostic never equips a weapon.
const char* KHZ_EquipmentServerJSON(unsigned slot,std::uint64_t session,std::uint64_t humanGeneration);
const char* KHZ_EquipmentClientJSON(std::uint64_t session,std::uint64_t ownerGeneration);
bool KHZ_EquipmentReconnectEligible(std::uint64_t session,std::uint64_t ownerGeneration);

// Source-owned EOF khz_client_owner.cpp fragment. Caller must already hold the
// acknowledged main controls barrier; direct presentation mirror reads refuse.
struct KHZ_EquipmentClientFrame {
    std::uint64_t session,generation,tick;
    int serverId,currentSnapshot,nextSnapshot,currentTime,nextTime;
    playerState_s accepted,predicted;
};
bool KHZ_ClientOwnerReadEquipment(KHZ_EquipmentClientFrame& result,
    std::uint64_t expectedSession,std::uint64_t expectedGeneration);
