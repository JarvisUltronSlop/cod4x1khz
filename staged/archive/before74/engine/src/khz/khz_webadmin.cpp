#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "khz_webadmin.h"
#include <webadmin_policy.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "ws2_32.lib")

namespace {
namespace web = khz::web;
enum Phase { Free, Pending, Processing, Complete };
struct Slot { std::atomic<int> phase{Free}; web::Request request{}; web::Reply reply{}; };
std::array<Slot, web::MaxConnections> slots{};
std::shared_ptr<const web::View> published;
std::atomic<std::uint64_t> epoch{1}, accepted{0}, rejected{0}, completed{0}, loginFailed{0}, socketTimeouts{0};
std::atomic<std::uint64_t> ownerServices{0}, ownerMaximumUs{0}, ownerOverruns{0};
std::atomic<bool> stopping{false}, running{false};
std::thread worker;
web::Config configuration;
std::uint64_t connectionSerial = 0;
struct Connection {
    SOCKET socket = INVALID_SOCKET;
    std::uint64_t generation = 0, deadline = 0;
    std::uint32_t peer = 0;
    std::string received, sending;
    std::size_t sent = 0;
    bool waiting = false;
};
std::array<Connection, web::MaxConnections> connections{};
struct LoginBucket { std::uint32_t peer = 0; std::uint64_t next = 0; };
std::array<LoginBucket, 64> loginBuckets{};
std::uint64_t nextLogin = 0;
void copy(char* output, std::size_t capacity, std::string_view value) {
    if (!capacity) return; const auto length = (std::min)(capacity - 1, value.size());
    std::memcpy(output, value.data(), length); output[length] = 0;
}
bool nonblocking(SOCKET socket) { unsigned long enabled = 1; return ioctlsocket(socket, FIONBIO, &enabled) == 0; }
void closeSocket(Connection& connection) {
    if (connection.socket != INVALID_SOCKET) closesocket(connection.socket);
    connection.socket = INVALID_SOCKET; connection.received.clear(); connection.sending.clear(); connection.sent = 0;
}
std::string pageStart(std::string_view title) {
    return "<!doctype html><html><head><meta charset=utf-8><title>cod4x1khz " + web::Escape(title) +
        "</title><style>body{font:16px system-ui;margin:2rem;max-width:80rem;background:#14191f;color:#eee}a{color:#73c1ff}table{border-collapse:collapse}td,th{padding:.5rem;border:1px solid #59616a}input,button{padding:.5rem;margin:.25rem}pre{white-space:pre-wrap}form{margin:1rem 0}</style></head><body><nav><a href=/status>Status</a> | <a href=/webadmin>Administration</a></nav>";
}
std::string csrfField(std::string_view csrf) { return "<input type=hidden name=csrf value=\"" + web::Escape(csrf) + "\">"; }
std::string render(const web::View& view, const web::Reply& reply, bool publicStatus) {
    std::string body = pageStart(publicStatus ? "status" : "administration");
    body += "<h1>" + web::Escape(view.hostname) + "</h1><p>" + web::Escape(view.map) + " / " + web::Escape(view.gametype) + "</p>";
    if (!publicStatus && !reply.authorized) {
        if (*reply.text) body += "<p>" + web::Escape(reply.text) + "</p>";
        body += "<form method=post action=/webadmin><label>User <input name=username maxlength=31 required></label><label>Password <input name=password type=password maxlength=128 required></label><button>Log in</button></form></body></html>"; return body;
    }
    if (reply.authorized) body += "<p>Signed in as " + web::Escape(reply.name) + " (power " + std::to_string(reply.power) + ")</p>";
    body += "<table><tr><th>Slot</th><th>Name</th><th>Provider</th><th>Score</th><th>Ping</th>";
    if (reply.authorized) body += "<th>Local key</th><th>Power</th><th>Actions</th>";
    body += "</tr>";
    for (unsigned i = 0; i != view.players.size(); ++i) {
        const auto& player = view.players[i]; if (!player.connected || (player.undercover && (!reply.authorized || reply.power < 60))) continue;
        body += "<tr><td>" + std::to_string(i) + "</td><td>" + web::Escape(player.name) + "</td><td>" + (player.bot ? "native bot" : "local key") + "</td><td>" + std::to_string(player.score) + "</td><td>" + std::to_string(player.ping) + "</td>";
        if (reply.authorized) {
            char identity[65]; khz::identity::EncodeHex(player.admission.identity.data(), player.admission.identity.size(), identity, sizeof(identity));
            body += "<td>" + web::Escape(identity) + "</td><td>" + std::to_string(player.power) + "</td><td>";
            for (const char* action : {"kickclient", "banclient"}) {
                if (player.bot && !std::strcmp(action, "banclient")) continue;
                body += "<form method=post action=\"/webadmin?action=" + std::string(action) + "\">" + csrfField(reply.csrf) +
                    "<input type=hidden name=cid value=" + std::to_string(i) + "><input type=hidden name=generation value=" + std::to_string(player.admission.generation) +
                    "><input type=hidden name=identity value=" + std::string(identity) + "><input type=hidden name=bot value=" + (player.bot ? "1" : "0") + "><input name=reason maxlength=126 placeholder=Reason><button>" + action + "</button></form>";
            }
            body += "</td>";
        }
        body += "</tr>";
    }
    body += "</table>";
    if (reply.authorized) {
        if (reply.power >= 80) {
            body += "<h2>Administrators</h2><table><tr><th>Name</th><th>Power</th></tr>";
            for (const auto& admin : view.administrators) if (admin.power && admin.power <= reply.power)
                body += "<tr><td>" + web::Escape(admin.name) + "</td><td>" + std::to_string(admin.power) + "</td></tr>";
            body += "</table>";
        }
        body += "<form method=post action=\"/webadmin?action=sendcmd\">" + csrfField(reply.csrf) + "<input name=consolecommand maxlength=959 size=60 placeholder=Command required><button>Run</button></form>";
        body += "<form method=post action=\"/webadmin?action=banclient\">" + csrfField(reply.csrf) + "<input name=identity maxlength=64 size=64 placeholder=Full-local-key required><input name=reason maxlength=126 placeholder=Reason required><button>Ban offline key</button></form>";
        body += "<form method=post action=\"/webadmin?action=logout\">" + csrfField(reply.csrf) + "<button>Log out</button></form>";
        if (*reply.text) body += "<pre>" + web::Escape(reply.text) + "</pre>";
    }
    body += "<p>Custom protocol10002. Official identity provider unavailable.</p></body></html>";
    return body;
}
void respond(Connection& connection, int status, std::string body, const char* type = "text/html; charset=utf-8", const web::Reply* reply = nullptr) {
    if (body.size() > web::MaxReply) { status = 503; body = "Bounded response capacity exceeded"; ++rejected; }
    const char* phrase = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 408 ? "Request Timeout" : status == 409 ? "Conflict" : status == 413 ? "Payload Too Large" : status == 429 ? "Too Many Requests" : "Service Unavailable";
    connection.sending = "HTTP/1.1 " + std::to_string(status) + " " + phrase + "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    if (reply && (reply->session[0] || reply->clearCookie)) connection.sending += "Set-Cookie: SessionId=" + std::string(reply->session) + "; Path=/; HttpOnly; SameSite=Strict" + (reply->clearCookie ? "; Max-Age=0" : "; Max-Age=3600") + "\r\n";
    connection.sending += "\r\n" + body; connection.sent = 0;
}
bool staticResource(std::string_view relative, std::string* bytes, const char** type) {
    if (!web::StaticPath(relative) || !configuration.staticRoot[0]) return false;
    std::error_code error; const auto root = std::filesystem::absolute(configuration.staticRoot, error).lexically_normal(); if (error) return false;
    std::filesystem::path path;
    for (const auto& part : root) { path /= part; const auto flags = GetFileAttributesW(path.c_str()); if (flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return false; }
    const auto suffix = std::filesystem::path(std::string(relative));
    for (const auto& part : suffix) { path /= part; const auto flags = GetFileAttributesW(path.c_str()); if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return false; }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct FileGuard { HANDLE file; ~FileGuard() { CloseHandle(file); } } guard{file};
    LARGE_INTEGER size{}; wchar_t finalPath[1024]; const DWORD count = GetFinalPathNameByHandleW(file, finalPath, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    bool valid = count && count < 1024 && !std::wcsncmp(finalPath, L"\\\\?\\", 4) && !_wcsicmp(finalPath + 4, path.c_str()) &&
        GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= web::MaxReply;
    if (valid) { bytes->resize(std::size_t(size.QuadPart)); DWORD read = 0; valid = ReadFile(file, bytes->data(), DWORD(bytes->size()), &read, nullptr) && read == bytes->size(); }
    *type = relative.ends_with(".css") ? "text/css" : relative.ends_with(".js") ? "application/javascript" : relative.ends_with(".svg") ? "image/svg+xml" : "application/octet-stream";
    return valid;
}
void admit(unsigned index, const web::Parsed& parsed) {
    auto& connection = connections[index];
    if (!web::SameOrigin(parsed, configuration.bind, configuration.port)) { respond(connection, 403, "Configured host/origin required"); ++rejected; return; }
    const auto view = std::atomic_load(&published);
    if (!view) { respond(connection, 503, "Server view not ready"); return; }
    if (!parsed.post && parsed.target == "/status") { respond(connection, 200, render(*view, {}, true)); return; }
    if (!parsed.post && parsed.target == "/") { respond(connection, 200, pageStart("server") + "<h1>cod4x1khz</h1></body></html>"); return; }
    if (!parsed.post && parsed.target.starts_with("/files/")) {
        std::string bytes; const char* type = nullptr;
        if (staticResource(std::string_view(parsed.target).substr(7), &bytes, &type)) respond(connection, 200, std::move(bytes), type);
        else respond(connection, 404, "Resource unavailable"); return;
    }
    if (parsed.target != "/webadmin" && parsed.target != "/webadmin/") { respond(connection, 404, "Unknown route"); return; }
    web::Request request; request.slot = index; request.generation = connection.generation; request.epoch = epoch.load(); request.deadline = connection.deadline;
    request.serverId = view->serverId; request.serverSession = view->serverSession;
    std::string token; if (!web::Cookie(parsed.cookie, &token)) { respond(connection, 400, "Invalid session cookie"); return; }
    copy(request.session, sizeof(request.session), token);
    copy(request.csrf, sizeof(request.csrf), web::Value(parsed.form, "csrf"));
    const auto& action = web::Value(parsed.query, "action");
    if (!parsed.post) { if (!action.empty()) { respond(connection, 400, "Actions require POST"); return; } request.action = web::Action::Page; }
    else if (action.empty()) {
        request.action = web::Action::Login;
        const auto now = GetTickCount64(); LoginBucket* bucket = nullptr;
        for (auto& entry : loginBuckets) if (entry.peer == connection.peer) { bucket = &entry; break; }
        if (!bucket) { bucket = &loginBuckets[0]; for (auto& entry : loginBuckets) if (entry.next < bucket->next) bucket = &entry; *bucket = {connection.peer, 0}; }
        if (now < nextLogin || now < bucket->next) { respond(connection, 429, "Login rate limited"); ++rejected; return; }
        nextLogin = now + 100; bucket->next = now + 1000;
        const auto& name = web::Value(parsed.form, "username"); const auto& password = web::Value(parsed.form, "password");
        if (name.empty() || name.size() > 31 || password.size() < 8 || password.size() > 128) { respond(connection, 401, "Invalid login"); ++loginFailed; return; }
        for (const auto& admin : view->administrators) if (admin.power && web::Equal(admin.name, name)) { request.credential = admin; break; }
        auto verification = request.credential;
        if (!verification.power) { verification.power = 1; verification.passwordHash[0] = 1; }
        const bool validPassword = khz::admin::VerifyCredential(verification, password);
        request.passwordVerified = request.credential.power && validPassword;
        if (!request.passwordVerified) { bucket->next = GetTickCount64() + 20000; ++loginFailed; }
    } else {
        if (action == "logout") request.action = web::Action::Logout;
        else if (action == "sendcmd") { request.action = web::Action::Command; const auto& command = web::Value(parsed.form, "consolecommand"); if (command.size() > 959) { respond(connection, 400, "Command too long"); return; } copy(request.text, sizeof(request.text), command); }
        else if (action == "kickclient" || action == "banclient") {
            request.action = action == "kickclient" ? web::Action::Kick : web::Action::Ban;
            const auto& identity = web::Value(parsed.form, "identity");
            request.targetBot = web::Value(parsed.form, "bot") == "1";
            if (!khz::identity::DecodeHex(identity, request.target.identity.data(), request.target.identity.size()) ||
                (khz::identity::IsEmpty(request.target.identity) && !(request.targetBot && request.action == web::Action::Kick))) { respond(connection, 400, "Full proven key required"); return; }
            const auto& cid = web::Value(parsed.form, "cid");
            if (cid.empty() && request.action == web::Action::Ban) request.action = web::Action::OfflineBan;
            else {
                std::uint64_t target = 0, generation = 0;
                if (!web::Decimal(cid, 15, &target) || !web::Decimal(web::Value(parsed.form, "generation"), UINT64_MAX, &generation) || !generation) { respond(connection, 400, "Admission required"); return; }
                request.targetSlot = int(target); request.target.generation = generation;
            }
            const auto& reason = web::Value(parsed.form, "reason");
            if (reason.size() > 126 || (request.action != web::Action::Kick && reason.empty())) { respond(connection, 400, "Bounded ban reason required"); return; }
            copy(request.text, sizeof(request.text), reason.empty() ? "The admin has no reason given" : reason);
        } else { respond(connection, 400, "Unknown action"); return; }
    }
    auto& slot = slots[index];
    if (slot.phase.load(std::memory_order_acquire) != Free) { respond(connection, 503, "Owner queue busy"); ++rejected; return; }
    slot.request = request; slot.phase.store(Pending, std::memory_order_release); connection.waiting = true; ++accepted;
}
void work() {
    WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data)) return;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(configuration.port);
    bool bound = listener != INVALID_SOCKET && inet_pton(AF_INET, configuration.bind, &address.sin_addr) == 1;
    if (bound) { const BOOL exclusive = TRUE; setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)); bound = !bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) && !listen(listener, 16) && nonblocking(listener); }
    if (!bound) { if (listener != INVALID_SOCKET) closesocket(listener); WSACleanup(); ++rejected; return; }
    running.store(true, std::memory_order_release); auto observedEpoch = epoch.load();
    try { while (!stopping.load(std::memory_order_acquire)) {
        if (observedEpoch != epoch.load()) {
            for (unsigned i = 0; i != connections.size(); ++i) {
                closeSocket(connections[i]);
                int expected = Pending;
                slots[i].phase.compare_exchange_strong(expected, Free, std::memory_order_acq_rel);
                // Processing requests remain owned until Complete acknowledges
                // them. Reusing a slot here races the owner's copied request.
                connections[i].waiting = slots[i].phase.load(std::memory_order_acquire) != Free;
            }
            observedEpoch = epoch.load();
        }
        sockaddr_in from{}; int bytes = sizeof(from); SOCKET incoming = accept(listener, reinterpret_cast<sockaddr*>(&from), &bytes);
        if (incoming != INVALID_SOCKET) {
            unsigned free = connections.size();
            for (unsigned i = 0; i != connections.size(); ++i) if (connections[i].socket == INVALID_SOCKET && !connections[i].waiting && slots[i].phase.load() == Free) { free = i; break; }
            if (free == connections.size() || !nonblocking(incoming)) { closesocket(incoming); ++rejected; }
            else { auto& connection = connections[free]; connection = {}; connection.socket = incoming; connection.generation = ++connectionSerial; connection.deadline = GetTickCount64() + 15000; connection.peer = from.sin_addr.s_addr; }
        }
        for (unsigned i = 0; i != connections.size(); ++i) {
            auto& connection = connections[i]; auto& slot = slots[i];
            try {
            if (connection.waiting && slot.phase.load(std::memory_order_acquire) == Complete) {
                const auto reply = slot.reply; const auto view = std::atomic_load(&published);
                const bool current = slot.request.epoch == epoch.load(std::memory_order_acquire);
                slot.phase.store(Free, std::memory_order_release); connection.waiting = false;
                if (!current) closeSocket(connection);
                else if (connection.socket != INVALID_SOCKET && view) respond(connection, reply.status, render(*view, reply, false), "text/html; charset=utf-8", &reply);
            }
            if (connection.socket == INVALID_SOCKET) continue;
            if (GetTickCount64() > connection.deadline) { closeSocket(connection); ++socketTimeouts; continue; }
            if (!connection.sending.empty()) {
                const int sent = send(connection.socket, connection.sending.data() + connection.sent, int(connection.sending.size() - connection.sent), 0);
                if (sent > 0) connection.sent += std::size_t(sent);
                else if (!sent || WSAGetLastError() != WSAEWOULDBLOCK) closeSocket(connection);
                if (connection.socket != INVALID_SOCKET && connection.sent == connection.sending.size()) closeSocket(connection);
                continue;
            }
            if (connection.waiting) continue;
            char buffer[4096]; const int received = recv(connection.socket, buffer, sizeof(buffer), 0);
            if (received > 0) {
                connection.received.append(buffer, received); web::Parsed parsed;
                const auto status = web::Parse(connection.received, &parsed);
                if (status == web::ParseResult::Valid) admit(i, parsed);
                else if (status != web::ParseResult::Incomplete) { ++rejected; respond(connection, status == web::ParseResult::TooLarge ? 413 : 400, "Invalid bounded HTTP request"); }
            } else if (!received || WSAGetLastError() != WSAEWOULDBLOCK) closeSocket(connection);
            } catch (...) { closeSocket(connection); ++rejected; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } } catch (...) { ++rejected; }
    for (auto& connection : connections) closeSocket(connection);
    closesocket(listener); running.store(false, std::memory_order_release); WSACleanup();
}
}
bool KHZ_WebadminStart(const web::Config& config) {
    if (!web::PrivateAddress(config.bind) || config.port < 1024) return false;
    KHZ_WebadminShutdown(); configuration = config; stopping.store(false); ++epoch;
    try { worker = std::thread(work); return true; } catch (...) { stopping.store(true); return false; }
}
void KHZ_WebadminPublish(std::shared_ptr<const web::View> view) { std::atomic_store(&published, std::move(view)); }
bool KHZ_WebadminPoll(web::Request* request) {
    if (!request) return false;
    for (auto& slot : slots) {
        int expected = Pending;
        if (!slot.phase.compare_exchange_strong(expected, Processing, std::memory_order_acq_rel)) continue;
        if (slot.request.epoch != epoch.load(std::memory_order_acquire) || GetTickCount64() > slot.request.deadline) {
            slot.reply = {}; slot.phase.store(Complete, std::memory_order_release); continue;
        }
        *request = slot.request; return true;
    }
    return false;
}
void KHZ_WebadminComplete(const web::Request& request, const web::Reply& reply) {
    if (request.slot >= slots.size()) return;
    auto& slot = slots[request.slot];
    if (slot.phase.load(std::memory_order_acquire) != Processing || slot.request.generation != request.generation) return;
    slot.reply = reply; slot.phase.store(Complete, std::memory_order_release); ++completed;
}
void KHZ_WebadminShutdown() {
    stopping.store(true, std::memory_order_release); if (worker.joinable()) worker.join(); running.store(false); ++epoch;
    for (auto& slot : slots) slot.phase.store(Free, std::memory_order_release);
    for (auto& connection : connections) connection = {};
    std::atomic_store(&published, std::shared_ptr<const web::View>{});
}
void KHZ_WebadminErrorCleanup() {
    ++epoch;
    // Called by the same owner after aborting its service stack. A worker may
    // discard Pending, but only this owner can acknowledge Processing.
    for (auto& slot : slots) if (slot.phase.load(std::memory_order_acquire) == Processing) {
        slot.reply = {}; slot.phase.store(Complete, std::memory_order_release);
    }
}
bool KHZ_WebadminRunning() { return running.load(std::memory_order_acquire); }
const char* KHZ_WebadminStatsJSON() {
    static char text[512]; std::snprintf(text, sizeof(text), "{\"running\":%s,\"requests\":%llu,\"completed\":%llu,\"rejected\":%llu,\"failed_logins\":%llu,\"timeouts\":%llu,\"owner_services\":%llu,\"owner_max_us\":%llu,\"owner_overruns\":%llu}", running.load() ? "true" : "false", accepted.load(), completed.load(), rejected.load(), loginFailed.load(), socketTimeouts.load(), ownerServices.load(), ownerMaximumUs.load(), ownerOverruns.load()); return text;
}
void KHZ_WebadminRecordOwnerWork(std::uint64_t microseconds) {
    ++ownerServices; if (microseconds >= 1000) ++ownerOverruns;
    auto maximum = ownerMaximumUs.load(); while (maximum < microseconds && !ownerMaximumUs.compare_exchange_weak(maximum, microseconds)) {}
}
