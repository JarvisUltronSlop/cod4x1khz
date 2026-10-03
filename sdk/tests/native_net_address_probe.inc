/* Real rebuilt-DLL address ABI, export and native admission fixtures. */
static unsigned net_cases, net_failures, net_connects, net_reserved;
static unsigned net_connect_layout, net_reserved_layout, net_reserved_granted;
static netadr_t net_last_connect, net_last_reserved;
static CONVAR_T* net_allow_reserved;
static void net_check(int condition, const char* name) {
    ++net_cases; if (!condition) { ++net_failures; Plugin_PrintError("KHZ_PLUGIN_NET_CASE failed=%s\n", name); }
}
static unsigned net_host_port(unsigned short value) { return ((unsigned)value >> 8) | (((unsigned)value & 255) << 8); }
static int net_layout_valid(const netadr_t* address) {
    return address && sizeof(*address) == 32 && address->pad == 0 && address->scope_id == 0 &&
        (address->type == NA_LOOPBACK || (address->sock != 0 && address->sock != -1)) &&
        (address->type == NA_IP || address->type == NA_LOOPBACK);
}
static void net_fixture_probe(void) {
    netadr_t v4, v6, second, mapped, failed;
    char text[128], tiny[10]; unsigned i;
    net_cases = net_failures = 0;
    net_check(COD4X1KHZ_PLUGIN_SDK_VERSION == 2 && sizeof(netadr_t) == 32 && offsetof(netadr_t, ip) == 16,
        "sdk2_public_layout");
    net_check(Plugin_NET_StringToAdr("192.0.2.44:65535", &v4, NA_IP) == 1 && v4.type == NA_IP &&
        v4.ip[0] == 192 && v4.ip[1] == 0 && v4.ip[2] == 2 && v4.ip[3] == 44 && net_host_port(v4.port) == 65535,
        "native_numeric_ipv4_unsigned_port");
    net_check(Plugin_NET_StringToAdr("[2001:db8:1234::1]:32768", &v6, NA_IP6) == 1 && v6.type == NA_IP6 &&
        v6.ip6[0] == 0x20 && v6.ip6[1] == 1 && v6.ip6[2] == 0x0d && v6.ip6[3] == 0xb8 &&
        v6.ip6[4] == 0x12 && v6.ip6[5] == 0x34 && v6.ip6[15] == 1 && net_host_port(v6.port) == 32768,
        "native_numeric_ipv6_public_bytes");
    net_check(!strcmp(Plugin_NET_AdrToString(&v4), "192.0.2.44:65535"), "native_nonmt_full");
    net_check(!strcmp(Plugin_NET_AdrToStringShort(&v4), "192.0.2.44"), "native_nonmt_short");
    net_check(Plugin_NET_AdrToStringMT(&v6, text, sizeof(text)) == text && !strcmp(text, "[2001:db8:1234::1]:32768"),
        "native_mt_ipv6_full");
    Plugin_NET_AdrToStringShortMT(&v6, text, sizeof(text)); net_check(!strcmp(text, "2001:db8:1234::1"), "native_mt_ipv6_short");
    Plugin_NET_AdrMaskToStringMT(&v4, text, sizeof(text)); net_check(!strcmp(text, "192.0.x.x:65535"), "native_mt_ipv4_mask");
    Plugin_NET_AdrMaskToStringMT(&v6, text, sizeof(text)); net_check(!strcmp(text, "[2001:0db8:1234:x:x:x:x:x]:32768"), "native_mt_ipv6_mask");
    memset(tiny, 'X', sizeof(tiny)); Plugin_NET_AdrToStringMT(&v4, tiny + 1, 8);
    net_check(tiny[0] == 'X' && tiny[9] == 'X' && !strcmp(tiny + 1, "buf < N"), "native_mt_buffer_bounds");
    net_check(!strcmp(Plugin_NET_AdrToString(NULL), "(null)"), "native_null_format_literal");
    second = v4; second.port ^= 1;
    net_check(Plugin_NET_CompareBaseAdr(&v4, &second) && !Plugin_NET_CompareAdr(&v4, &second), "native_full_vs_base");
    second = v4; second.ip[3] ^= 1;
    net_check(Plugin_NET_CompareBaseAdrMask(&v4, &second, 31) && !Plugin_NET_CompareBaseAdrMask(&v4, &second, 32) &&
        !Plugin_NET_CompareBaseAdrMask(&v4, &second, -1) && !Plugin_NET_CompareBaseAdrMask(&v4, &second, 33),
        "native_ipv4_mask_boundaries");
    second = v6; second.ip6[15] ^= 1;
    net_check(Plugin_NET_CompareBaseAdrMask(&v6, &second, 127) && !Plugin_NET_CompareBaseAdrMask(&v6, &second, 128) &&
        !Plugin_NET_CompareBaseAdrMask(&v6, &second, -1) && !Plugin_NET_CompareBaseAdrMask(&v6, &second, 129),
        "native_ipv6_mask_boundaries");
    second = v4; second.type = NA_TCP;
    net_check(!Plugin_NET_CompareBaseAdrMask(&v4, &second, 0), "native_exact_family_comparison");
    net_check(Plugin_NET_StringToAdr("127.0.0.1", &mapped, NA_IP6) == 2 && mapped.ip6[10] == 255 &&
        mapped.ip6[11] == 255 && mapped.ip6[12] == 127 && mapped.ip6[15] == 1 && net_host_port(mapped.port) == 28960,
        "native_forced_v6_mapped_literal_default_port");
    net_check(Plugin_NET_StringToAdr("fe80::1%7", &mapped, NA_IP6) == 2 && mapped.scope_id == 7, "native_numeric_scope");
    Plugin_NET_AdrToStringMT(&mapped, text, sizeof(text)); net_check(!strcmp(text, "[fe80::1%7]:28960"), "native_scope_format");
    net_check(Plugin_IsLANAddress(&mapped), "native_ipv6_link_local_lan");
    Plugin_NET_StringToAdr("fd00::1", &mapped, NA_IP6); net_check(Plugin_IsLANAddress(&mapped), "native_ipv6_ula_lan");
    Plugin_NET_StringToAdr("10.1.2.3", &mapped, NA_IP); net_check(Plugin_IsLANAddress(&mapped), "native_ipv4_private_lan");
    net_check(!Plugin_IsLANAddress(&v6), "native_documentation_ipv6_non_lan");
    memset(&failed, 0xff, sizeof(failed));
    net_check(Plugin_NET_StringToAdr("localhost:28960", &failed, NA_UNSPEC) == 0 && failed.type == NA_BAD && !failed.port,
        "native_dns_pending_never_reported_success");
    memset(&failed, 0xff, sizeof(failed));
    net_check(Plugin_NET_StringToAdr("[::1]:65536", &failed, NA_UNSPEC) == 0 && failed.type == NA_BAD && !failed.port,
        "native_malformed_port_clears_output");
    for (i = 0; i < 16; ++i) net_check(Plugin_NET_CompareBaseAdrMask(&v6, &v6, (int)i * 8), "native_export_prefix_self");
    Plugin_Printf("KHZ_PLUGIN_NET_FIXTURE cases=%u failures=%u sdk=%d connects=%u connect_layout=%u reserved=%u reserved_layout=%u grants=%u\n",
        net_cases, net_failures, COD4X1KHZ_PLUGIN_SDK_VERSION, net_connects, net_connect_layout, net_reserved, net_reserved_layout, net_reserved_granted);
}
static void net_fixture_admission_report(void) {
    char connect[128], reserved[128];
    Plugin_NET_AdrToStringMT(&net_last_connect, connect, sizeof(connect));
    Plugin_NET_AdrToStringMT(&net_last_reserved, reserved, sizeof(reserved));
    Plugin_Printf("KHZ_PLUGIN_NET_ADMISSION connects=%u connect_layout=%u reserved=%u reserved_layout=%u grants=%u connect=%s reserved_address=%s\n",
        net_connects, net_connect_layout, net_reserved, net_reserved_layout, net_reserved_granted, connect, reserved);
}
static void net_fixture_ipban_probe(void) {
    netadr_t a, b, ipv6; char message[1024], bounded[12];
    unsigned before = net_cases, beforeFailures = net_failures;
    Plugin_NET_StringToAdr("192.0.2.198:65535", &a, NA_IP);
    Plugin_NET_StringToAdr("192.0.2.199:32768", &b, NA_IP);
    Plugin_NET_StringToAdr("2001:db8::198", &ipv6, NA_IP6);
    Plugin_RemoveBanByip(&a); Plugin_RemoveBanByip(&b);
    Plugin_PlayerAddBanByip(&a, "KHZ public32 IP fixture", (int)Plugin_GetRealtime() + 60);
    net_check(Plugin_PlayerBannedByip(&a, message, sizeof(message)) == message &&
        strstr(message, "KHZ public32 IP fixture\nThis is a temporary ban and will expire in\n"), "native_public32_ipban_remaining_text");
    net_check(!Plugin_PlayerBannedByip(&b, message, sizeof(message)) && !message[0], "native_ipban_distinct_address");
    a.port ^= 1; net_check(Plugin_PlayerBannedByip(&a, message, sizeof(message)) == message, "native_ipban_base_address_ignores_port");
    memset(bounded, 'X', sizeof(bounded));
    net_check(Plugin_PlayerBannedByip(&a, bounded + 1, 10) == bounded + 1 && bounded[0] == 'X' && bounded[11] == 'X' && bounded[10] == 0,
        "native_ipban_message_bounds");
    Plugin_PlayerAddBanByip(&ipv6, "MUST reject unsupported IPv6", (int)Plugin_GetRealtime() + 60);
    net_check(!Plugin_PlayerBannedByip(&ipv6, message, sizeof(message)) && !message[0], "native_ipv6_ipban_pending_without_truncation");
    net_check(Plugin_PlayerBannedByip(&a, message, sizeof(message)) == message, "native_rejected_ipv6_cannot_replace_ipv4_ban");
    Plugin_RemoveBanByip(&a); net_check(!Plugin_PlayerBannedByip(&a, message, sizeof(message)) && !message[0], "native_ipban_remove_exact_address");
    Plugin_PlayerAddBanByip(&b, "KHZ permanent bounded NAT fixture", -1);
    net_check(Plugin_PlayerBannedByip(&b, message, sizeof(message)) == message && strstr(message, "0 days 4 hours") &&
        !strstr(message, "This is a permanent ban."),
        "native_ipban_permanent_request_bounded_four_hours");
    Plugin_RemoveBanByip(&b);
    Plugin_Printf("KHZ_PLUGIN_NET_IPBAN cases=%u failures=%u cleanup=1\n", net_cases - before, net_failures - beforeFailures);
}
PCL void OnPlayerConnect(int slot, netadr_t* address, char* guid, char* userinfo, int authstatus, char* denied, int capacity) {
    (void)slot; (void)guid; (void)userinfo; (void)authstatus; (void)denied; (void)capacity;
    ++net_connects; if (net_layout_valid(address)) ++net_connect_layout;
    if (address) net_last_connect = *address;
}
PCL void OnPlayerWantReservedSlot(netadr_t* address, char* guid, char* userinfo, int authstatus, qboolean* allowed) {
    (void)guid; (void)userinfo; (void)authstatus;
    ++net_reserved; if (net_layout_valid(address)) ++net_reserved_layout;
    if (address) net_last_reserved = *address;
    if (allowed && net_allow_reserved && Plugin_Cvar_GetBoolean(net_allow_reserved)) { *allowed = qtrue; ++net_reserved_granted; }
}
