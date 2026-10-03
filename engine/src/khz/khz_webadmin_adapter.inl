// Included by khz_admin_server.cpp after its owner state/functions. The HTTP
// worker never reads Store or client_t; only this service may mutate them.
namespace {
namespace web = khz::web;
const dvar_t *webEnabled = nullptr, *webBind = nullptr, *webPort = nullptr;
web::Config webConfiguration{};
bool webConfigured = false;
std::uint64_t webRetry = 0, webPublishAt = 0, webPublishedSession = 0;
int webPublishedServerId = 0;
web::Reply* webRedirect = nullptr;
void clearWebadminOwnerContext() { webRedirect = nullptr; currentInvoker = {{}, true}; redirectSlot = -1; remoteConsoleContext = false; }

void initWebadmin() {
    webEnabled = Dvar_RegisterBool("sv_webadmin", false, DVAR_ARCHIVE, "Enable project private HTTP administration outside gameplay ticks");
    webBind = Dvar_RegisterString("khz_webadmin_bind", "127.0.0.1", DVAR_ARCHIVE, "Numeric loopback/private IPv4 interface for HTTP administration");
    webPort = Dvar_RegisterInt("khz_webadmin_port", 28980, 1024, 65535, DVAR_ARCHIVE, "Private HTTP administration port");
}
void webRedirectOutput(char* output) {
    if (!webRedirect || !output) return;
    const auto used = std::strlen(webRedirect->text), available = sizeof(webRedirect->text) - used - 1;
    const auto length = (std::min)(std::strlen(output), available);
    std::memcpy(webRedirect->text + used, output, length); webRedirect->text[used + length] = 0;
}
bool csrfFor(const char* session, char (&csrf)[65]) {
    id::Fingerprint digest{};
    return session && web::HexToken(session) && id::Sha256(session, 64, digest) &&
        id::EncodeHex(digest.data(), digest.size(), csrf, sizeof(csrf));
}
void fillWebAuthorization(web::Reply& reply, const policy::AdminRecord& account, const char* session) {
    reply.authorized = true; reply.power = account.power;
    I_strncpyz(reply.name, account.name, sizeof(reply.name)); csrfFor(session, reply.csrf);
}
bool webTarget(const web::Request& request, client_t** result) {
    *result = nullptr;
    if (!svs.clients || !sv_maxclients || request.targetSlot < 0 || request.targetSlot >= KHZ_MaxMatchClients ||
        request.targetSlot >= sv_maxclients->current.integer) return false;
    auto* client = &svs.clients[request.targetSlot];
    if (client->header.state < CS_CONNECTED || clients[request.targetSlot].generation != request.target.generation ||
        KHZ_IsNativeBot(client) != request.targetBot) return false;
    const auto& fingerprint = clients[request.targetSlot].fingerprint;
    if (request.targetBot ? !id::IsEmpty(request.target.identity) :
        !policy::SameAdmission(request.target, {fingerprint, clients[request.targetSlot].generation})) return false;
    *result = client; return true;
}
void executeWebadmin(const web::Request& request, web::Reply& reply) {
    if (!storageReady || request.serverSession != KHZ_CurrentServerSession() || request.serverId != sv_serverId_value ||
        GetTickCount64() > request.deadline) {
        reply.status = 409; I_strncpyz(reply.text, "Server lifecycle changed; retry from its current page.", sizeof(reply.text)); return;
    }
    if (request.action == web::Action::Login) {
        const auto* session = request.passwordVerified ? store.AdmitVerifiedCredential(request.credential, unixTime()) : nullptr;
        if (!session) { reply.status = 401; reply.clearCookie = true; I_strncpyz(reply.text, "Invalid or changed administrator credentials.", sizeof(reply.text)); return; }
        id::EncodeHex(session->token.data(), session->token.size(), reply.session, sizeof(reply.session));
        const auto* account = store.FindAdmin(session->identity); if (account) fillWebAuthorization(reply, *account, reply.session);
        const auto saved = currentInvoker; currentInvoker = {session->identity, false}; auditEvent("webadmin_login"); currentInvoker = saved; return;
    }
    const auto* account = store.SessionAdmin(request.session, unixTime());
    if (!account) { reply.status = request.action == web::Action::Page ? 200 : 401; reply.clearCookie = request.session[0] != 0; return; }
    // Copy before a command can remove/change this account in Store.
    const auto credential = *account; fillWebAuthorization(reply, credential, request.session);
    if (request.action == web::Action::Page) return;
    if (!csrfFor(request.session, reply.csrf) || !policy::ConstantTimeEqual(request.csrf, reply.csrf)) {
        reply.status = 403; I_strncpyz(reply.text, "Session CSRF token required.", sizeof(reply.text)); return;
    }
    const auto savedInvoker = currentInvoker; const int savedSlot = redirectSlot;
    currentInvoker = {credential.identity, false}; redirectSlot = -1;
    if (request.action == web::Action::Logout) {
        store.WipeSessions(credential.identity); reply.authorized = false; reply.clearCookie = true; auditEvent("webadmin_logout");
    } else if (request.action == web::Action::Command) {
        char name[64];
        if (!policy::ValidateCommand(request.text, name, sizeof(name)) || !invokerCanExecute(request.text, -1)) {
            reply.status = 403; ++deniedCommands; I_strncpyz(reply.text, "Command unavailable or insufficient current power.", sizeof(reply.text));
        } else {
            char output[4096], command[1024]; I_strncpyz(command, request.text, sizeof(command));
            webRedirect = &reply; Com_BeginRedirect(output, sizeof(output), webRedirectOutput);
            ++acceptedCommands; Cmd_ExecuteSingleCommand(0, 0, command); Com_EndRedirect(); webRedirect = nullptr;
            // Log command name only: password and player-supplied arguments
            // must never become an authentication/audit side channel.
            auditEvent("webadmin_command", name);
        }
    } else {
        client_t* target = nullptr;
        const bool offline = request.action == web::Action::OfflineBan;
        const char* rule = request.action == web::Action::Kick ? "kick" : "permban";
        if (!invokerCanExecute(rule, -1) || (offline ? (id::IsEmpty(request.target.identity) || request.targetBot) : !webTarget(request, &target))) {
            reply.status = 403; ++deniedCommands; I_strncpyz(reply.text, "Target admission changed or command power denied.", sizeof(reply.text));
        } else if (target && KHZ_AdminClientPower(target) > store.Power(currentInvoker)) {
            reply.status = 403; I_strncpyz(reply.text, "Target has higher administration power.", sizeof(reply.text));
        } else if (request.action == web::Action::Kick) {
            auditEvent("webadmin_kick", target->name); SV_DropClient(target, va("Kicked: %s", request.text), true);
            I_strncpyz(reply.text, "Player disconnected.", sizeof(reply.text));
        } else {
            const auto subject = subjectFor(target, request.target.identity);
            const auto result = store.AddBan(currentInvoker, subject, target ? target->name : "offline", request.text, unixTime(), 0, false);
            if (result != policy::Result::Ok) { reply.status = 403; I_strncpyz(reply.text, resultText(result), sizeof(reply.text)); }
            else {
                char full[65]; id::EncodeHex(subject.identity.data(), subject.identity.size(), full, sizeof(full));
                auditEvent("webadmin_permban", full); persist();
                if (target) { char message[1024]; KHZ_AdminFormatBanMessage(-1, request.text, message, sizeof(message)); SV_DropClient(target, message, true); }
                I_strncpyz(reply.text, "Identity ban recorded.", sizeof(reply.text));
            }
        }
    }
    currentInvoker = savedInvoker; redirectSlot = savedSlot;
    // Account mutations revoke sessions immediately, including the issuer's.
    if (reply.authorized && !store.SessionAdmin(request.session, unixTime())) { reply.authorized = false; reply.clearCookie = true; }
}
void publishWebadmin(std::uint64_t now) {
    auto view = std::make_shared<web::View>(); view->serverSession = KHZ_CurrentServerSession(); view->serverId = sv_serverId_value;
    I_strncpyz(view->hostname, sv_hostname ? sv_hostname->current.string : "cod4x1khz", sizeof(view->hostname));
    I_strncpyz(view->map, sv_mapname ? sv_mapname->current.string : "", sizeof(view->map));
    I_strncpyz(view->gametype, sv_gametype ? sv_gametype->current.string : "", sizeof(view->gametype));
    view->administrators = store.Data().admins;
    if (svs.clients && sv_maxclients) for (int i = 0; i < (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer); ++i) {
        const auto& client = svs.clients[i]; auto& player = view->players[i]; player.connected = client.header.state >= CS_CONNECTED;
        if (!player.connected) continue;
        player.bot = KHZ_IsNativeBot(&client); player.undercover = KHZ_AdminClientUndercover(&client);
        player.admission = {clients[i].fingerprint, clients[i].generation}; player.power = KHZ_AdminClientPower(&client);
        player.ping = player.bot ? 0 : client.ping; I_strncpyz(player.name, client.name, sizeof(player.name));
        if (client.gentity && client.gentity->client) { player.score = client.gentity->client->sess.score; player.team = client.gentity->client->sess.cs.team; }
    }
    KHZ_WebadminPublish(std::move(view)); webPublishAt = now + 100;
}
void serviceWebadmin() {
    if (!webEnabled || !storageReady || !com_sv_running || !com_sv_running->current.enabled || !webEnabled->current.enabled) {
        if (webConfigured) KHZ_WebadminShutdown(); webConfigured = false; webRetry = webPublishAt = 0; return;
    }
    struct Measurement {
        const std::uint64_t start = khz::counter();
        ~Measurement() { const auto frequency = khz::frequency(); if (frequency) KHZ_WebadminRecordOwnerWork((khz::counter() - start) * 1000000 / frequency); }
    } measurement;
    const auto now = GetTickCount64(); web::Config next;
    const auto root = std::filesystem::path(KHZ_HomeRoot()) / "webadmindata";
    if (!webBind || !webPort || !web::PrivateAddress(webBind->current.string) || root.wstring().size() >= std::size(next.staticRoot)) {
        if (webConfigured) KHZ_WebadminShutdown(); webConfigured = false; return;
    }
    I_strncpyz(next.bind, webBind->current.string, sizeof(next.bind)); next.port = static_cast<unsigned short>(webPort->current.integer);
    std::wcscpy(next.staticRoot, root.c_str());
    const bool changed = !webConfigured || next.port != webConfiguration.port || std::strcmp(next.bind, webConfiguration.bind);
    if (changed || (!KHZ_WebadminRunning() && now >= webRetry)) {
        webConfigured = KHZ_WebadminStart(next); webConfiguration = next; webRetry = now + 30000; webPublishAt = 0;
        if (webConfigured) Com_Printf(CON_CHANNEL_SERVER, "Private HTTP administration starting at http://%s:%u/webadmin\n", next.bind, next.port);
    }
    const bool lifecycleChanged = webPublishedSession != KHZ_CurrentServerSession() || webPublishedServerId != sv_serverId_value;
    if (lifecycleChanged) { KHZ_WebadminErrorCleanup(); webPublishedSession = KHZ_CurrentServerSession(); webPublishedServerId = sv_serverId_value; webPublishAt = 0; }
    if (!webConfigured) return;
    if (now >= webPublishAt) publishWebadmin(now);
    for (unsigned drained = 0; drained != 4; ++drained) {
        web::Request request; if (!KHZ_WebadminPoll(&request)) break;
        web::Reply reply; executeWebadmin(request, reply); KHZ_WebadminComplete(request, reply);
    }
}
}
