#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
namespace khz::identity {
namespace {
cmd_function_s guidProbeCommand{};

bool guidProbeUnrelatedCrypto() {
    constexpr std::array<std::uint8_t, 32> expected = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    Fingerprint digest{};
    if (!Sha256("abc", 3, digest) || digest != expected) return false;
    PrivateKey key; PublicKey publicKey{}; Fingerprint fingerprint{}, verified{};
    Challenge challenge{}; Proof proof{};
    challenge.challenge = 1; challenge.protocol = 10002; challenge.qport = 28982;
    challenge.boot.fill(0x31); challenge.nonce.fill(0x72);
    if (!GenerateKey(key) || !ExportPublicKey(key, publicKey) ||
        !PublicKeyFingerprint(publicKey, fingerprint) || !Sign(key, challenge, proof) ||
        proof.publicKey != publicKey || !Verify(proof, challenge, verified) || verified != fingerprint) return false;
    ++challenge.nonce[0];
    return !Verify(proof, challenge, verified) && IsEmpty(verified);
}

void guidProbeRun() {
    // Gate before installing TLS or invoking any cryptographic operation.
    if (!Sys_IsMainThread() || com_inServerFrame || Cmd_Argc() != 1 ||
        !Dvar_GetBool("khz_benchmark") || !Dvar_GetBool("khz_test_controls") || guidProbeState) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_IDENTITY_GUID_PROVIDER rejected=1\n");
        return;
    }
    unsigned cases = 0, failures = 0, cryptoIsolationCases = 0;
    unsigned openAttempts = 0, opened = 0, deriveAttempts = 0, derived = 0;
    unsigned closeAttempts = 0, closed = 0, closeErrors = 0;
    constexpr char input[] = "0123456789abcdef0123456789abcdef";
    constexpr std::uint64_t golden = 2310346614158149229ULL;
    // All cryptographic calls and their RAII cleanup finish before printing.
    for (unsigned phase = 0; phase != 4; ++phase) {
        GuidProbeState state{};
        state.fault = phase == 1 ? GuidFault::Open : phase == 2 ? GuidFault::Derive : GuidFault::None;
        std::uint64_t converted = 0;
        bool isolated = true;
        {
            GuidProbeScope scope(state);
            if (phase == 1 || phase == 2) {
                isolated = guidProbeUnrelatedCrypto();
                ++cryptoIsolationCases;
                // The unrelated real crypto may not consume GUID counters.
                isolated = isolated && !state.openAttempts && !state.deriveAttempts &&
                    !state.opened && !state.derived && !state.closeAttempts && !state.closed;
            }
            converted = GuidToPlayerId(input);
        }
        const bool openFailure = phase == 1, deriveFailure = phase == 2;
        ++cases;
        if (!isolated || converted != (openFailure || deriveFailure ? 0 : golden) ||
            state.openAttempts != 1 || state.opened != (openFailure ? 0u : 1u) ||
            state.deriveAttempts != (openFailure ? 0u : 1u) ||
            state.derived != (openFailure || deriveFailure ? 0u : 1u) ||
            state.closeAttempts != state.opened || state.closed != state.opened ||
            state.closeErrors || guidProbeState) ++failures;
        openAttempts += state.openAttempts; opened += state.opened;
        deriveAttempts += state.deriveAttempts; derived += state.derived;
        closeAttempts += state.closeAttempts; closed += state.closed; closeErrors += state.closeErrors;
    }
    Com_Printf(CON_CHANNEL_DONT_FILTER,
        "KHZ_IDENTITY_GUID_PROVIDER cases=%u failures=%u open_failure=1 derive_failure=1 "
        "normal_before=1 normal_after=1 isolation_cases=%u open_attempts=%u opened=%u "
        "derive_attempts=%u derived=%u close_attempts=%u closed=%u close_errors=%u "
        "live_handles=%u scope_reset=%u provider_authentication=0\n",
        cases, failures, cryptoIsolationCases, openAttempts, opened, deriveAttempts,
        derived, closeAttempts, closed, closeErrors, opened - closed, guidProbeState ? 0u : 1u);
}
}
}

bool KHZ_InitIdentityProbe() {
    if (!Sys_IsMainThread()) return false;
    static bool installed = false;
    if (!installed) {
        Cmd_AddCommandInternal("khz_identity_guid_provider", khz::identity::guidProbeRun,
            &khz::identity::guidProbeCommand);
        installed = true;
    }
    return true;
}
#endif
