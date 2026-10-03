/* Include in regression_plugin.c after its standard includes. The native
 * commands require an isolated proven human profile. Each changed stat is
 * restored before reporting either success or failure. */
static uint32_t received_stats_hash(int slot) {
    uint32_t value = 2166136261u;
    int index, octet;
    for (index = 0; index != 3498; ++index) {
        uint32_t stat = (uint32_t)Plugin_GetStat(slot, index);
        for (octet = 0; octet != (index < 2000 ? 1 : 4); ++octet) {
            value = (value ^ (stat & 255u)) * 16777619u;
            stat >>= 8;
        }
    }
    return value;
}
static void stats_probe(void) {
    const int fields[] = {0, 1999, 2000, 2301, 3497};
    const unsigned int replacements[] = {37u, 219u, 0x89abcdefu, 54321u, 0xfedcba98u};
    int originals[5], slot, i, passed = 1;
    uint32_t before, after;
    char* end;
    if (Plugin_Cmd_Argc() != 2) { Plugin_Printf("Usage: khz_fixture_stats <connected isolated human slot>\n"); return; }
    slot = (int)strtol(Plugin_Cmd_Argv(1), &end, 10);
    if (*end || slot < 0 || slot >= Plugin_GetSlotCount() || !Plugin_GetPlayerID((unsigned)slot)) {
        Plugin_Printf("KHZ_PLUGIN_STATS_FIXTURE requires a proven human admission\n"); return;
    }
    before = received_stats_hash(slot);
    for (i = 0; i != 5; ++i) originals[i] = Plugin_GetStat(slot, fields[i]);
    for (i = 0; i != 5; ++i) {
        Plugin_SetStat(slot, fields[i], (int)replacements[i]);
        if ((unsigned int)Plugin_GetStat(slot, fields[i]) != replacements[i]) passed = 0;
        Plugin_SetStat(slot, fields[i], originals[i]);
        if (Plugin_GetStat(slot, fields[i]) != originals[i]) passed = 0;
    }
    Plugin_SetStat(slot, -1, 1); Plugin_SetStat(slot, 3498, 1); Plugin_SetStat(slot, 0, 256);
    if (Plugin_GetStat(slot, -1) || Plugin_GetStat(slot, 3498)) passed = 0;
    for (i = 0; i != 5; ++i) if (Plugin_GetStat(slot, fields[i]) != originals[i]) passed = 0;
    after = received_stats_hash(slot); passed &= before == after;
    Plugin_Printf("KHZ_PLUGIN_STATS_FIXTURE slot=%d roundtrip=%d restored=%d fields=0,1999,2000,2301,3497 invalid_rejected=%d all_received_fields=%d before_hash=%u after_hash=%u\n", slot, passed, passed, passed, before == after, before, after);
    if (!passed) Plugin_PrintError("KHZ_PLUGIN_STATS_FIXTURE FAILED native received-stat layout/read/write\n");
}
static void identity_probe(void) {
    const uint64_t steam = (1ULL << 56) | (1ULL << 52) | (1ULL << 32) | 12345ULL;
    const uint64_t guid = 2310346614158149229ULL;
    char text[128], shortBuffer[3] = {'x', 'x', 'x'};
    const int decimal = Plugin_Cvar_VariableBooleanValue("sv_usesteam64id");
    int passed = Plugin_StringToSteamID("[U:1:12345]") == steam &&
        Plugin_StringToSteamID("STEAM_1:1:6172") == steam && Plugin_StringToSteamID("invalid") == 0 &&
        Plugin_StringToSteamID("18446744073709551616") == 0 &&
        Plugin_GUID2PlayerID("0123456789abcdef0123456789abcdef") == guid &&
        Plugin_GUID2PlayerID("0123456789ABCDEF0123456789ABCDEF") == guid && Plugin_GUID2PlayerID("invalid") == 0;
    Plugin_SteamIDToString(steam, text, sizeof(text)); if (strcmp(text, decimal ? "76561197960278073" : "[U:1:12345]")) passed = 0;
    Plugin_SteamIDTo64String(steam, text, sizeof(text)); if (Plugin_StringToSteamID(text) != steam) passed = 0;
    Plugin_SteamIDToString(steam, shortBuffer, sizeof(shortBuffer));
    if (decimal ? (shortBuffer[0] || shortBuffer[1] != 'x' || shortBuffer[2] != 'x') : shortBuffer[sizeof(shortBuffer) - 1]) passed = 0;
    if (!Plugin_SteamIDIsIndividual(steam) || !Plugin_SteamIDIsIndividualAndSteamAccount(steam) ||
        !Plugin_SteamIDIsIndividual(guid) || Plugin_SteamIDIsIndividualAndSteamAccount(guid)) passed = 0;
    Plugin_Printf("KHZ_PLUGIN_IDENTITY_FIXTURE conversions=%d guid_golden=%llu official_provider=unavailable\n", passed, guid);
    if (!passed) Plugin_PrintError("KHZ_PLUGIN_IDENTITY_FIXTURE FAILED public conversion semantics\n");
}
