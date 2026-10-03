/* Actual rebuilt-export boundary tests. No invocation of the old unsafe
 * implementation is necessary. All native allocation/I/O is outside ticks. */
static int identity_parse_guard_call(const char* input, uint64_t expected,
                                     size_t accessible, unsigned int* faults) {
    unsigned char original[129];
    uint64_t actual = 0;
    int faulted = 0;
    if (!input || accessible > sizeof(original)) return 0;
    memcpy(original, input, accessible);
    __try { actual = Plugin_StringToSteamID(input); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        faulted = 1;
        ++*faults;
    }
    return !faulted && actual == expected && !memcmp(original, input, accessible);
}
static void identity_parse_guard_pages(struct identity_contract_counts* result,
                                       unsigned int* faults) {
    SYSTEM_INFO info;
    unsigned char* pages;
    char* edge;
    DWORD previous = 0;
    GetSystemInfo(&info);
    if (info.dwPageSize < 129 || info.dwPageSize > 65536) {
        identity_contract_check(result, 0, "guard_page_size");
        return;
    }
    pages = (unsigned char*)VirtualAlloc(NULL, (SIZE_T)info.dwPageSize * 2,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pages) {
        identity_contract_check(result, 0, "guard_page_allocation");
        return;
    }
    if (!VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous)) {
        identity_contract_check(result, 0, "guard_page_protection");
        identity_contract_check(result, VirtualFree(pages, 0, MEM_RELEASE) != 0, "guard_page_release");
        return;
    }
    edge = (char*)pages + info.dwPageSize;
    memcpy(edge - 18, "76561197960265770", 18);
    identity_contract_check(result, identity_parse_guard_call(edge - 18,
        0x011000010000002aULL, 18, faults), "guard_terminated_prefix");
    edge[-1] = 0;
    identity_contract_check(result, identity_parse_guard_call(edge - 1, 0, 1, faults),
        "guard_empty_terminated_prefix");
    memset(edge - 128, '1', 128);
    identity_contract_check(result, identity_parse_guard_call(edge - 128, 0, 128, faults),
        "guard_128_nonterminated_bytes");
    memset(edge - 128, '0', 110);
    memcpy(edge - 18, "76561197960265770", 18);
    identity_contract_check(result, identity_parse_guard_call(edge - 128,
        0x011000010000002aULL, 128, faults), "guard_127_length_terminated");
    memset(edge - 129, '0', 111);
    memcpy(edge - 18, "76561197960265770", 18);
    identity_contract_check(result, identity_parse_guard_call(edge - 129, 0, 129, faults),
        "guard_128_length_terminated");
    identity_contract_check(result, VirtualFree(pages, 0, MEM_RELEASE) != 0,
        "guard_page_release");
}
