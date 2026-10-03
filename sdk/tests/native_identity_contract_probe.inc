/* Actual rebuilt-DLL fixture source: native execution awaits the next build.
 * Invoke through khz_fixture_identity classify|guid, preserving twenty command
 * registrations. This calls real exports; the oracles assemble input fields
 * independently instead of importing the implementation helpers. The adjacent
 * GUID literal data was independently generated and cross-reviewed offline.
 */
struct identity_contract_counts { unsigned int cases, failures; };
static void identity_contract_check(struct identity_contract_counts* counts,
                                    int passed, const char* label) {
    ++counts->cases;
    if (!passed) {
        ++counts->failures;
        /* Keep failure reporting bounded even for a faulty entire matrix. */
        if (counts->failures <= 8)
            Plugin_PrintError("KHZ_IDENTITY_CONTRACT_FAILURE %s\n", label);
    }
}

static void identity_classify_pair(struct identity_contract_counts* individual,
                                  struct identity_contract_counts* steam,
                                  uint64_t id, int expectedIndividual,
                                  int expectedSteam, const char* label) {
    identity_contract_check(individual,
        Plugin_SteamIDIsIndividual(id) == expectedIndividual, label);
    identity_contract_check(steam,
        Plugin_SteamIDIsIndividualAndSteamAccount(id) == expectedSteam, label);
}

static void identity_classify_probe(void) {
    static const unsigned int instances[] = {0, 1, 2, 0xfffff};
    static const unsigned int accounts[] = {0, 0xffffffffU};
    static const uint64_t literals[] = {
        0ULL, UINT64_MAX, 76561197960278073ULL,
        2310346614158149229ULL, 76561193665310777ULL,
        81064797587648569ULL
    };
    static const int literalIndividual[] = {0, 0, 1, 1, 0, 0};
    static const int literalSteam[] = {0, 0, 1, 0, 0, 0};
    struct identity_contract_counts individual = {0, 0}, steam = {0, 0};
    unsigned int universe, type, instance, account, bit, literal;
    const uint64_t base = 76561197960278016ULL; /* U:1:0, instance one. */
    for (universe = 0; universe != 256; ++universe)
        for (type = 0; type != 16; ++type)
            for (instance = 0; instance != 4; ++instance)
                for (account = 0; account != 2; ++account) {
                    const uint64_t id = ((uint64_t)universe << 56) |
                        ((uint64_t)type << 52) |
                        ((uint64_t)instances[instance] << 32) | accounts[account];
                    const int eligible = type == 1 && instances[instance] == 1;
                    /* Expectations use original field inputs, not decoded id. */
                    const int publicUniverse = universe == 1;
                    const int privateUniverse = universe >= 32 && universe <= 39;
                    identity_classify_pair(&individual, &steam, id,
                        eligible && (publicUniverse || privateUniverse),
                        eligible && publicUniverse, "assembled_field_matrix");
                }
    for (bit = 0; bit != 64; ++bit) {
        /* Account bits have no classification effect. Flipping universe bit
         * 61 maps public universe one to private universe 33. Every other
         * upper-bit flip leaves the individually eligible set. */
        identity_classify_pair(&individual, &steam, base ^ (1ULL << bit),
            bit < 32 || bit == 61, bit < 32, "independent_single_bit_oracle");
    }
    for (literal = 0; literal != 6; ++literal)
        identity_classify_pair(&individual, &steam, literals[literal],
            literalIndividual[literal], literalSteam[literal], "literal_goldens");
    Plugin_Printf("KHZ_IDENTITY_INDIVIDUAL cases=%u failures=%u matrix=32768 bit_flips=64 literals=6 provider_authentication=0\n",
        individual.cases, individual.failures);
    Plugin_Printf("KHZ_IDENTITY_STEAM_ACCOUNT cases=%u failures=%u matrix=32768 bit_flips=64 literals=6 provider_authentication=0\n",
        steam.cases, steam.failures);
}

struct identity_guid_golden { const char* text; uint64_t expected; };
#include "identity_guid_goldens.inc" /* Independent Python/.NET oracle data. */

static int identity_guid_readonly(const char* text, uint64_t expected) {
    unsigned char storage[96], original[96];
    char* input = (char*)storage + 8;
    size_t length = strlen(text);
    if (length >= sizeof(storage) - 8) return 0;
    memset(storage, 0xa5, sizeof(storage));
    memcpy(input, text, length + 1);
    memcpy(original, storage, sizeof(storage));
    return Plugin_GUID2PlayerID(input) == expected &&
        memcmp(storage, original, sizeof(storage)) == 0;
}

static void identity_guid_probe(void) {
    static const char invalid[] = {'g', '/', ' ', '\t', '\n'};
    struct identity_contract_counts guid = {0, 0};
    char input[80];
    unsigned int golden, variant, position, replacement, length;
    const unsigned int goldens = (unsigned int)(sizeof(identity_guid_goldens) /
                                                sizeof(identity_guid_goldens[0]));
    for (golden = 0; golden != goldens; ++golden)
        for (variant = 0; variant != 3; ++variant) {
            memcpy(input, identity_guid_goldens[golden].text, 33);
            for (position = 0; position != 32; ++position)
                if ((variant == 1 || (variant == 2 && (position & 1))) &&
                    input[position] >= 'a' && input[position] <= 'f')
                    input[position] = (char)(input[position] - 'a' + 'A');
            identity_contract_check(&guid, identity_guid_readonly(input,
                identity_guid_goldens[golden].expected), "independent_pbkdf2_golden_case");
        }
    for (position = 0; position != 32; ++position)
        for (replacement = 0; replacement != sizeof(invalid); ++replacement) {
            memset(input, '0', 32); input[32] = '\0';
            input[position] = invalid[replacement];
            identity_contract_check(&guid, identity_guid_readonly(input, 0),
                "reject_nonhex_at_every_byte_without_mutating_input");
        }
    for (length = 0; length <= 64; ++length) {
        if (length == 32) continue;
        memset(input, '0', length); input[length] = '\0';
        identity_contract_check(&guid, identity_guid_readonly(input, 0),
            "reject_wrong_length_without_mutating_input");
    }
    identity_contract_check(&guid, Plugin_GUID2PlayerID(NULL) == 0,
        "null_input_returns_zero");
    /* The API takes a NUL-terminated string, so bytes beyond a valid terminator
     * are not part of the identifier. Observe those bytes remain unchanged. */
    memset(input, 'z', sizeof(input));
    memcpy(input, identity_guid_goldens[0].text, 33);
    identity_contract_check(&guid,
        Plugin_GUID2PlayerID(input) == identity_guid_goldens[0].expected && input[33] == 'z',
        "valid_terminated_prefix_ignores_following_bytes");
    Plugin_Printf("KHZ_IDENTITY_GUID cases=%u failures=%u goldens=%u case_variants=3 invalid_hex=160 wrong_lengths=64 null=1 terminated_prefix=1 provider_authentication=0 provider_failure_injected=0\n",
        guid.cases, guid.failures, goldens);
}
