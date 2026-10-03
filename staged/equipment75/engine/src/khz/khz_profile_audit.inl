// Include inside existing profile.cpp anonymous namespace BEFORE CommitUnlocks.
// Required TU headers: khz_identity.h, khz_client_owner.h, khz_profile_audit.h,
// unlock_audit_policy.hpp and qcommon/threads.h. Root applies wiring separately.
unsigned char unlockAuditBytes[khz::unlock_audit::Capacity];
unsigned char unlockAuditTables[khz::unlock_audit::TableCapacity];
unsigned unlockAuditSize=0;
std::uint64_t unlockAuditSerial=0,unlockAuditNextSerial=0;
char unlockAuditHome[1024],unlockAuditProfile[96],unlockAuditBackup[96];
char unlockAuditFailure[128]="no_completed_transaction";

bool CaptureStockTables(khz::unlock_audit::Bytes& bytes){
    bytes.u32(3);
    for(const char* name:{"mp/statstable.csv","mp/attachmenttable.csv","mp/ranktable.csv"}){
        const auto* table=Table(name,1);
        if(!table||table->columnCount<1||table->columnCount>32||table->rowCount<2||table->rowCount>1024)return false;
        if(!bytes.text(name,95))return false;bytes.u32(table->rowCount);bytes.u32(table->columnCount);
        for(int row=0;row<table->rowCount;++row)for(int column=0;column<table->columnCount;++column)
            if(!bytes.text(Cell(table,row,column),1023))return false;
    }return bytes.ok();
}
void CaptureSuccessfulUnlockAudit(const playerStatNetworkData& original,
    const std::array<unsigned char,StatsBytes>& candidate,const UnlockPlan& plan,const UnlockRecord& record,
    const StatsFile& encoded,const std::filesystem::path& profile,const std::filesystem::path& backup){
    // Normal interactive operation has no audit export path. This copy happens
    // outside gameplay only in an explicit isolated benchmark+controls fixture.
    if(!KHZ_IsBenchmarkHome()||!Dvar_GetBool("khz_benchmark")||!Dvar_GetBool("khz_test_controls"))return;
    unlockAuditSize=0;unlockAuditSerial=0;
    if(!Sys_IsMainThread()||KHZ_IsClientSimulationThread()){
        I_strncpyz(unlockAuditFailure,"requires_actual_main_transaction_owner",sizeof(unlockAuditFailure));return;
    }
    const auto home=std::string(KHZ_HomeRoot()),name=std::string(com_playerProfile->current.string),backupName=backup.filename().string();
    if(home.size()>=sizeof(unlockAuditHome)||name.size()>=sizeof(unlockAuditProfile)||backupName.size()>=sizeof(unlockAuditBackup)||
        profile.filename().string()!=name||backup.parent_path()!=profile/"khz_backups"){
        I_strncpyz(unlockAuditFailure,"actual_home_profile_backup_identity",sizeof(unlockAuditFailure));return;
    }
    std::vector<unsigned char> saved,sidecar,committed;
    if(!ReadBytes(backup/"memory_stats.bin",&saved,StatsBytes)||saved.size()!=StatsBytes||
        std::memcmp(saved.data(),original.playerStats,StatsBytes)||
        !ReadBytes(profile/UnlockRecordFilename,&sidecar,sizeof(record))||sidecar.size()!=sizeof(record)||std::memcmp(sidecar.data(),&record,sizeof(record))||
        !ReadBytes(profile/"mpdata",&committed,sizeof(encoded))||committed.size()!=sizeof(encoded)||std::memcmp(committed.data(),&encoded,sizeof(encoded))){
        I_strncpyz(unlockAuditFailure,"actual_completed_backup_and_commit_files_disagree",sizeof(unlockAuditFailure));return;
    }
    khz::unlock_audit::Bytes tables(unlockAuditTables,sizeof(unlockAuditTables));
    if(!CaptureStockTables(tables)){I_strncpyz(unlockAuditFailure,"actual_stock_tables_capacity_or_identity",sizeof(unlockAuditFailure));return;}
    const khz::unlock_audit::Evidence evidence{original.playerStats,candidate.data(),sidecar.data(),committed.data(),unlockAuditTables,
        plan.masks.data(),plan.minimumXp,tables.size(),home.c_str(),name.c_str(),backupName.c_str(),true,true,true};
    khz::unlock_audit::Bytes bytes(unlockAuditBytes,sizeof(unlockAuditBytes));
    const auto serial=++unlockAuditNextSerial;
    const auto error=khz::unlock_audit::serialize(bytes,serial,evidence);
    if(error!=khz::unlock_audit::Failure::None){I_strncpyz(unlockAuditFailure,khz::unlock_audit::failureName(error),sizeof(unlockAuditFailure));return;}
    I_strncpyz(unlockAuditHome,home.c_str(),sizeof(unlockAuditHome));I_strncpyz(unlockAuditProfile,name.c_str(),sizeof(unlockAuditProfile));I_strncpyz(unlockAuditBackup,backupName.c_str(),sizeof(unlockAuditBackup));
    unlockAuditSize=bytes.size();unlockAuditSerial=serial;I_strncpyz(unlockAuditFailure,"none",sizeof(unlockAuditFailure));
}

// Propose these public functions AFTER profile.cpp's anonymous namespace closes.
// No actual init/registration is performed by this proposal fragment.
