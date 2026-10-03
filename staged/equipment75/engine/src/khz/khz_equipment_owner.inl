// Proposal: include at EOF khz_client_owner.cpp, in the existing non-dedi MP
// compilation branch, after Owner storage and native routing implementations.
#include "khz_equipment_inspection.h"
bool KHZ_ClientOwnerReadEquipment(KHZ_EquipmentClientFrame& result,
    std::uint64_t expectedSession,std::uint64_t expectedGeneration) {
    if(!Sys_IsMainThread()||!controlAccess||simulationThread||!expectedSession||!expectedGeneration)
        return false;
    if(!owner.active.load(std::memory_order_acquire)||owner.phase.load(std::memory_order_acquire)!=Phase::Parked||
        owner.fault.load(std::memory_order_acquire)||owner.session!=expectedSession||owner.generation!=expectedGeneration||
        khz_clientUIActives[0].connectionState!=CA_ACTIVE||!khz_clientUIActives[0].cgameInitialized||
        !owner.game[0].snap||!owner.game[0].nextSnap||!owner.currentSnapshotNumber||
        !owner.publishedSnapshotNumber.load(std::memory_order_acquire))return false;
    // The worker is acknowledged parked; accepted gameplay snapshots and native
    // predicted PS belong to this storage, unlike frontend presentation data.
    result.session=owner.session;result.generation=owner.generation;
    result.tick=owner.tick.load(std::memory_order_relaxed);result.serverId=khz_clients[0].serverId;
    result.currentSnapshot=owner.currentSnapshotNumber;
    result.nextSnapshot=owner.publishedSnapshotNumber.load(std::memory_order_relaxed);
    result.currentTime=owner.game[0].snap->serverTime;result.nextTime=owner.game[0].nextSnap->serverTime;
    std::memcpy(&result.accepted,&owner.game[0].nextSnap->ps,sizeof(result.accepted));
    std::memcpy(&result.predicted,&owner.game[0].predictedPlayerState,sizeof(result.predicted));
    return result.tick>0&&result.serverId!=0&&result.nextSnapshot>=result.currentSnapshot&&
        result.nextTime>=result.currentTime&&result.accepted.clientNum==result.predicted.clientNum;
}
