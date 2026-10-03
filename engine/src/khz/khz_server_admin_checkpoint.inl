// Typed fixed-storage administration image. Dynamic path objects and OS file,
// credential-worker and web sockets are never copied or recreated from bytes.
#include "khz_server_admin_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include <type_traits>

namespace {
using namespace khz::server_native_checkpoint;
struct ServerAdminImage {
    KHZ_ServerCheckpointFrame frame;
    Bytes<sizeof(store)> store;
    Bytes<sizeof(rconLimiter)> limiter;
    Bytes<sizeof(ipBanCache)> ipBans;
    Bytes<sizeof(clients)> clients;
    Bytes<sizeof(pendingProofs)> proofs;
    Bytes<sizeof(bootNonce)> nonce;
    Bytes<sizeof(requests)> requests;
    Bytes<sizeof(pluginBans)> pluginBans;
    Bytes<sizeof(audit)> audit;
    policy::Invoker invoker;
    unsigned requestHead, pluginBanHead, auditHead;
    std::uint64_t auditDropped, deniedCommands, acceptedCommands, proofAccepted, proofRejected;
    std::uint64_t pluginBansAccepted, pluginBansRejected, pluginBansReusedSlot, generationSerial;
    wchar_t storagePath[4096], logPath[4096];
    bool captured;
};
ServerAdminImage serverAdminImage;
static_assert(std::is_trivially_copyable_v<decltype(store)>);
static_assert(std::is_trivially_copyable_v<decltype(rconLimiter)>);
static_assert(std::is_trivially_copyable_v<decltype(ipBanCache)>);
static_assert(std::is_trivially_copyable_v<decltype(clients)>);
static_assert(std::is_trivially_copyable_v<decltype(pendingProofs)>);
static_assert(std::is_trivially_copyable_v<policy::Invoker>);

bool serverAdminBoundary(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity)) return false;
    if (!initialized || !storageReady || !cryptoReady ||
        f.session != KHZ_CurrentServerSession() || f.logicalTick != KHZ_ServerTickNumber() || f.nativeTime != svs.time ||
        f.clientsAddress != reinterpret_cast<std::uintptr_t>(svs.clients) ||
        requestCount || pluginBanCount || auditCount || remoteConsoleContext || redirectSlot != -1 ||
        !currentInvoker.console || !id::IsEmpty(currentInvoker.identity) || currentInvoker.temporaryPower != -1)
        return reject(text, capacity, "server_admin_pending_external_work_or_invoker_context_is_active");
    // Native shutdown joins its sole socket/credential worker and clears its
    // requests before publishing Running=false on this same main owner.
    if (KHZ_WebadminRunning())
        return reject(text, capacity, "server_admin_web_or_credential_worker_is_active");
    const auto pathLength = std::wcslen(storagePath.c_str()), logLength = std::wcslen(logPath.c_str());
    if (pathLength >= 4096 || logLength >= 4096)
        return reject(text, capacity, "server_admin_immutable_storage_path_exceeds_fixture_bound");
    for (unsigned i = 0; i != clients.size(); ++i)
        if (svs.clients[i].header.state >= CS_CONNECTED &&
            KHZ_AdminClientGeneration(&svs.clients[i]) != clients[i].generation)
            return reject(text, capacity, "server_admin_native_admission_generation_is_invalid");
    return true;
}
bool serverAdminCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverAdminBoundary(f, text, capacity)) return false;
    auto& image = serverAdminImage; image.frame = f;
    image.store.capture(&store); image.limiter.capture(&rconLimiter); image.ipBans.capture(&ipBanCache);
    image.clients.capture(&clients); image.proofs.capture(&pendingProofs); image.nonce.capture(&bootNonce);
    image.requests.capture(&requests); image.pluginBans.capture(&pluginBans); image.audit.capture(&audit);
    image.invoker = currentInvoker;
    image.requestHead = requestHead; image.pluginBanHead = pluginBanHead; image.auditHead = auditHead;
    image.auditDropped = auditDropped; image.deniedCommands = deniedCommands; image.acceptedCommands = acceptedCommands;
    image.proofAccepted = proofAccepted; image.proofRejected = proofRejected;
    image.pluginBansAccepted = pluginBansAccepted; image.pluginBansRejected = pluginBansRejected;
    image.pluginBansReusedSlot = pluginBansReusedSlot; image.generationSerial = generationSerial;
    std::wcscpy(image.storagePath, storagePath.c_str()); std::wcscpy(image.logPath, logPath.c_str());
    image.captured = true; return true;
}
bool serverAdminValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverAdminBoundary(f, text, capacity)) return false;
    const auto& image = serverAdminImage;
    if (!image.captured || !khz::server_checkpoint::sameLifetime(f, image.frame) ||
        std::wcscmp(image.storagePath, storagePath.c_str()) || std::wcscmp(image.logPath, logPath.c_str()) ||
        std::memcmp(image.nonce.value, &bootNonce, sizeof(bootNonce)) || image.generationSerial != generationSerial)
        return reject(text, capacity, "server_admin_storage_nonce_or_admission_lifetime_changed");
    const auto& saved = *reinterpret_cast<const decltype(clients)*>(image.clients.value);
    for (unsigned i = 0; i != clients.size(); ++i)
        if (saved[i].generation != clients[i].generation || saved[i].fingerprint != clients[i].fingerprint)
            return reject(text, capacity, "server_admin_native_human_or_bot_admission_changed");
    return true;
}
void serverAdminRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext() && serverAdminImage.captured);
    const auto& image = serverAdminImage;
    image.store.restore(&store); image.limiter.restore(&rconLimiter); image.ipBans.restore(&ipBanCache);
    image.clients.restore(&clients); image.proofs.restore(&pendingProofs); image.nonce.restore(&bootNonce);
    image.requests.restore(&requests); image.pluginBans.restore(&pluginBans); image.audit.restore(&audit);
    currentInvoker = image.invoker; requestHead = image.requestHead; pluginBanHead = image.pluginBanHead; auditHead = image.auditHead;
    requestCount = pluginBanCount = auditCount = 0; redirectSlot = -1; remoteConsoleContext = false;
    auditDropped = image.auditDropped; deniedCommands = image.deniedCommands; acceptedCommands = image.acceptedCommands;
    proofAccepted = image.proofAccepted; proofRejected = image.proofRejected;
    pluginBansAccepted = image.pluginBansAccepted; pluginBansRejected = image.pluginBansRejected;
    pluginBansReusedSlot = image.pluginBansReusedSlot; generationSerial = image.generationSerial;
}
void serverAdminDiscard() { serverAdminImage.captured = false; }
}
bool KHZ_ServerAdminAllowExternalEffect(const char* reason) {
    if (!KHZ_ServerCheckpointPinned()) return true;
    KHZ_ServerCheckpointRejectExternalEffect(reason ? reason : "server_admin_external_effect"); return false;
}
void KHZ_ServerAdminRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Admin, 0, sizeof(serverAdminImage),
        serverAdminCapture, serverAdminValidate, serverAdminRestore, serverAdminDiscard};
    if (!KHZ_ServerCheckpointRegister(p)) Com_Error(ERR_FATAL, "Duplicate native server administration checkpoint provider");
}
