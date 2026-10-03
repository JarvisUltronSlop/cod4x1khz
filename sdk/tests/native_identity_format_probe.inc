/* Calls both real host exports, preserves the configured boolean, and reports
 * separate counters for complete per-export proof rather than aggregate names.
 */
struct identity_format_counts { int cases, failures; };
static void identity_format_check(struct identity_format_counts* counts, int passed, const char* name) {
    ++counts->cases;
    if (!passed) { ++counts->failures; Plugin_PrintError("KHZ_IDENTITY_FORMAT_FAILURE %s\n", name); }
}
static int identity_format_buffer(void (*format)(uint64_t, char*, int), uint64_t id, int capacity,
                                  const char* expected, int decimal) {
    unsigned char storage[80]; char* output = (char*)storage + 8;
    int index, copied, length = (int)strlen(expected);
    memset(storage, 0xa5, sizeof(storage));
    format(id, output, capacity);
    for (index = 0; index < (int)sizeof(storage); ++index)
        if ((index < 8 || capacity <= 0 || index >= 8 + capacity) && storage[index] != 0xa5) return 0;
    if (capacity <= 0) return 1;
    if (decimal && (capacity < 20 || length >= capacity)) {
        if (output[0]) return 0;
        for (index = 1; index < capacity; ++index) if ((unsigned char)output[index] != 0xa5) return 0;
        return 1;
    }
    copied = length < capacity ? length : capacity - 1;
    return !memcmp(output, expected, (size_t)copied) && output[copied] == 0;
}
static void identity_format_probe(void) {
    static const uint64_t ids[] = {0ULL, 76561197960278073ULL, 2310346613136044089ULL,
        9999999999999999999ULL, 10000000000000000000ULL, UINT64_MAX};
    static const char* decimal[] = {"0", "76561197960278073", "2310346613136044089",
        "9999999999999999999", "10000000000000000000", "18446744073709551615"};
    static const char* steam3[] = {"[I:0:0]", "[U:1:12345]", "[U:32:12345]", "[I:0:0]", "[I:0:0]", "[I:0:0]"};
    static const int capacities[] = {-1, 0, 1, 3, 19, 20, 21, 48};
    struct identity_format_counts selected = {0, 0}, explicit64 = {0, 0};
    struct identity_format_counts accountTypes = {0, 0};
    const int original = Plugin_Cvar_VariableBooleanValue("sv_usesteam64id");
    int mode, id, capacity, restored;
    for (mode = 0; mode <= 1; ++mode) {
        Plugin_Cvar_Set("sv_usesteam64id", mode ? "1" : "0");
        identity_format_check(&selected, Plugin_Cvar_VariableBooleanValue("sv_usesteam64id") == mode, "configured_mode_applied");
        for (id = 0; id < 6; ++id) for (capacity = 0; capacity < 8; ++capacity) {
            identity_format_check(&selected, identity_format_buffer(Plugin_SteamIDToString, ids[id], capacities[capacity],
                mode ? decimal[id] : steam3[id], mode), "selected_dvar_format_bounds");
            identity_format_check(&explicit64, identity_format_buffer(Plugin_SteamIDTo64String, ids[id], capacities[capacity],
                decimal[id], 1), "explicit64_ignores_mode_minimum_and_overflow_bounds");
        }
        /* Null outputs must be safe for both exports; negative/zero lengths
         * must not write. Sentinel tests above verify the non-null variants. */
        Plugin_SteamIDToString(ids[1], NULL, 21);
        Plugin_SteamIDTo64String(ids[1], NULL, 21);
        Plugin_SteamIDToString(ids[1], NULL, 0);
        Plugin_SteamIDTo64String(ids[1], NULL, 0);
        Plugin_SteamIDToString(ids[1], NULL, -1);
        Plugin_SteamIDTo64String(ids[1], NULL, -1);
        if (!mode) {
            static const unsigned int universes[] = {1, 2, 3, 4, 32, 33, 34, 35, 36, 37, 38, 39};
            static const unsigned int invalidUniverses[] = {0, 5, 31, 40, 255};
            static const unsigned int invalidTypes[] = {0, 11, 15};
            static const char typeNames[] = {'U', 'M', 'G', 'A', 'P', 'C', 'g', 'T', ' ', 'a'};
            int universe, type;
            char expected[48];
            for (universe = 0; universe < 12; ++universe) for (type = 1; type <= 10; ++type) {
                const uint64_t identifier = ((uint64_t)universes[universe] << 56) |
                    ((uint64_t)type << 52) | 0xffffffffULL;
                snprintf(expected, sizeof(expected), "[%c:%u:4294967295]", typeNames[type - 1], universes[universe]);
                identity_format_check(&accountTypes, identity_format_buffer(Plugin_SteamIDToString,
                    identifier, 48, expected, 0), "all_allowed_universes_and_account_types");
            }
            for (universe = 0; universe < 5; ++universe) {
                const uint64_t identifier = ((uint64_t)invalidUniverses[universe] << 56) | (1ULL << 52);
                identity_format_check(&accountTypes, identity_format_buffer(Plugin_SteamIDToString,
                    identifier, 48, "[I:0:0]", 0), "invalid_universe_is_invalid_steam3");
            }
            for (type = 0; type < 3; ++type) {
                const uint64_t identifier = (1ULL << 56) | ((uint64_t)invalidTypes[type] << 52);
                identity_format_check(&accountTypes, identity_format_buffer(Plugin_SteamIDToString,
                    identifier, 48, "[I:0:0]", 0), "invalid_account_type_is_invalid_steam3");
            }
        }
    }
    Plugin_Cvar_Set("sv_usesteam64id", original ? "1" : "0");
    restored = Plugin_Cvar_VariableBooleanValue("sv_usesteam64id") == original;
    identity_format_check(&selected, restored, "original_dvar_restored");
    Plugin_Printf("KHZ_IDENTITY_FORMAT_SELECTED cases=%d failures=%d initial=%d restored=%d provider_authentication=0\n",
        selected.cases, selected.failures, original, restored);
    Plugin_Printf("KHZ_IDENTITY_FORMAT_EXPLICIT64 cases=%d failures=%d provider_authentication=0\n",
        explicit64.cases, explicit64.failures);
    Plugin_Printf("KHZ_IDENTITY_FORMAT_STEAM3 cases=%d failures=%d provider_authentication=0\n",
        accountTypes.cases, accountTypes.failures);
}
