const char* KHZ_ProfileUnlockAuditJSON(){
    if(!Sys_IsMainThread())return "{\"schema\":1,\"complete\":false,\"reason\":\"requires_main_owner\"}";
    static char result[1024];
    if(!KHZ_IsBenchmarkHome()||!Dvar_GetBool("khz_benchmark")||!Dvar_GetBool("khz_test_controls"))return "{\"schema\":1,\"complete\":false,\"reason\":\"requires_isolated_controls\"}";
    std::snprintf(result,sizeof(result),"{\"schema\":1,\"complete\":%s,\"serial\":%llu,\"bytes\":%u,\"stats\":3498,\"raw_stats_bytes\":8192,\"reason\":\"%s\",\"full_weapon_acceptance\":false}",unlockAuditSerial&&unlockAuditSize?"true":"false",unlockAuditSerial,unlockAuditSize,unlockAuditFailure);return result;
}
const char* KHZ_ProfileUnlockAuditExportJSON(std::uint64_t expectedSerial){
    if(!Sys_IsMainThread())return "{\"schema\":1,\"committed\":false,\"reason\":\"requires_main_owner\"}";
    static char result[1024];
    const auto reject=[](const char* reason){std::snprintf(result,sizeof(result),"{\"schema\":1,\"committed\":false,\"reason\":\"%s\",\"full_weapon_acceptance\":false}",reason);return static_cast<const char*>(result);};
    if(!KHZ_IsBenchmarkHome()||!Dvar_GetBool("khz_benchmark")||!Dvar_GetBool("khz_test_controls")||
        KHZ_IsClientSimulationThread()||com_inServerFrame)return reject("requires_isolated_off_tick_main");
    if(!expectedSerial||unlockAuditSerial!=expectedSerial||!unlockAuditSize)return reject("expected_completed_audit_serial_mismatch");
    std::filesystem::path profile;std::string error;
    if(!ProfilePath(&profile,&error)||profile.filename().string()!=unlockAuditProfile||
        I_stricmp(KHZ_HomeRoot(),unlockAuditHome))return reject("current_home_profile_disagrees_with_audit");
    const auto filename=profile/"khz_unlock_audit.bin";
    if(GetFileAttributesW(filename.c_str())!=INVALID_FILE_ATTRIBUTES&&(GetFileAttributesW(filename.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))return reject("audit_destination_reparse_point");
    LARGE_INTEGER begin,end,frequency;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&begin);
    khz::identity::Fingerprint digest;char hex[65];
    if(!khz::identity::Sha256(unlockAuditBytes,unlockAuditSize,digest)||!khz::identity::EncodeHex(digest.data(),digest.size(),hex,sizeof(hex)))return reject("audit_digest_failed");
    if(!AtomicWrite(filename,unlockAuditBytes,unlockAuditSize))return reject("audit_atomic_file_commit_failed");
    QueryPerformanceCounter(&end);
    std::snprintf(result,sizeof(result),"{\"schema\":1,\"committed\":true,\"reason\":\"none\",\"serial\":%llu,\"file\":\"khz_unlock_audit.bin\",\"bytes\":%u,\"sha256\":\"%s\",\"elapsed_us\":%llu,\"immutable_transaction\":true,\"full_weapon_acceptance\":false,\"scope\":\"completed actual transaction and backup; diagnostic main off-tick file stall\"}",unlockAuditSerial,unlockAuditSize,hex,static_cast<unsigned long long>((end.QuadPart-begin.QuadPart)*1000000/frequency.QuadPart));return result;
}
const char* KHZ_ProfileSnapshotExportJSON(){
    if(!Sys_IsMainThread())return "{\"schema\":1,\"committed\":false,\"reason\":\"requires_main_owner\"}";
    static char result[1024];static unsigned char snapshot[StatsBytes];static std::uint64_t serial=0;
    const auto reject=[](const char* reason){std::snprintf(result,sizeof(result),"{\"schema\":1,\"committed\":false,\"reason\":\"%s\"}",reason);return static_cast<const char*>(result);};
    if(!KHZ_IsBenchmarkHome()||!Dvar_GetBool("khz_benchmark")||!Dvar_GetBool("khz_test_controls")||
        KHZ_IsClientSimulationThread()||com_inServerFrame||!LiveStorage_DoWeHaveStats())return reject("requires_isolated_loaded_off_tick_profile");
    std::filesystem::path profile;std::string error;if(!ProfilePath(&profile,&error))return reject("actual_profile_identity_invalid");
    std::memcpy(snapshot,LiveStorage_GetStatBuffer()->playerStats,sizeof(snapshot));
    const auto filename=profile/"khz_profile_snapshot.bin";
    if(GetFileAttributesW(filename.c_str())!=INVALID_FILE_ATTRIBUTES&&(GetFileAttributesW(filename.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))return reject("snapshot_destination_reparse_point");
    khz::identity::Fingerprint digest;char hex[65];
    if(!khz::identity::Sha256(snapshot,sizeof(snapshot),digest)||!khz::identity::EncodeHex(digest.data(),digest.size(),hex,sizeof(hex))||
        !AtomicWrite(filename,snapshot,sizeof(snapshot)))return reject("snapshot_atomic_commit_failed");
    std::snprintf(result,sizeof(result),"{\"schema\":1,\"committed\":true,\"reason\":\"none\",\"serial\":%llu,\"file\":\"khz_profile_snapshot.bin\",\"bytes\":8192,\"sha256\":\"%s\",\"full_weapon_acceptance\":false}",++serial,hex);return result;
}
