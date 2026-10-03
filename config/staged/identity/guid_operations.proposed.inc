// GUID-local CNG adapter. Shared Algorithm/P256/SHA256 callers do not use it.
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
enum class GuidFault { None, Open, Derive };
struct GuidProbeState {
    GuidFault fault = GuidFault::None;
    unsigned openAttempts = 0, opened = 0, deriveAttempts = 0, derived = 0;
    unsigned closeAttempts = 0, closed = 0, closeErrors = 0;
};
thread_local GuidProbeState* guidProbeState = nullptr;
struct GuidProbeScope {
    GuidProbeState* previous;
    explicit GuidProbeScope(GuidProbeState& state) : previous(guidProbeState) { guidProbeState = &state; }
    ~GuidProbeScope() { guidProbeState = previous; }
    GuidProbeScope(const GuidProbeScope&) = delete;
    GuidProbeScope& operator=(const GuidProbeScope&) = delete;
};
#endif

struct GuidAlgorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~GuidAlgorithm() {
        if (!handle) return;
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
        if (guidProbeState) ++guidProbeState->closeAttempts;
#endif
        const NTSTATUS status = BCryptCloseAlgorithmProvider(handle, 0);
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
        if (guidProbeState) {
            if (status >= 0) ++guidProbeState->closed;
            else ++guidProbeState->closeErrors;
        }
#else
        (void)status;
#endif
    }
};

NTSTATUS guidOpenAlgorithm(GuidAlgorithm& algorithm) {
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState) {
        ++guidProbeState->openAttempts;
        if (guidProbeState->fault == GuidFault::Open) return static_cast<NTSTATUS>(0xc0000001UL);
    }
#endif
    const NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm.handle,
        BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState && status >= 0) ++guidProbeState->opened;
#endif
    return status;
}

NTSTATUS guidDeriveKey(GuidAlgorithm& algorithm, std::array<std::uint8_t, 16>& decoded,
                       std::array<std::uint8_t, 4>& derived) {
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState) {
        ++guidProbeState->deriveAttempts;
        if (guidProbeState->fault == GuidFault::Derive) return static_cast<NTSTATUS>(0xc0000001UL);
    }
#endif
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(algorithm.handle, decoded.data(),
        ULONG(decoded.size()), const_cast<PUCHAR>(guidSalt), sizeof(guidSalt), 100,
        derived.data(), ULONG(derived.size()), 0);
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState && status >= 0) ++guidProbeState->derived;
#endif
    return status;
}
