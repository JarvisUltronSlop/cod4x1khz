#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <khz/khz_webadmin.h>
#include <webadmin_policy.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#pragma comment(lib, "ws2_32.lib")
#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); std::exit(1); } } while (0)
namespace web = khz::web;
namespace adm = khz::admin;
namespace id = khz::identity;
static unsigned short port;
static std::string exchange(const std::string& text) {
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(connection != INVALID_SOCKET);
    const DWORD timeout = 5000; setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    CHECK(!connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
    for (unsigned sent = 0; sent != text.size();) { const int count = send(connection, text.data() + sent, int(text.size() - sent), 0); CHECK(count > 0); sent += unsigned(count); }
    std::string received; char buffer[4096];
    for (;;) { const int count = recv(connection, buffer, sizeof(buffer), 0); if (!count) break; CHECK(count > 0); received.append(buffer, count); CHECK(received.size() <= web::MaxReply + 4096); }
    closesocket(connection); return received;
}
static std::string get(const char* path, const std::string& cookie = {}) {
    return "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n" + (cookie.empty() ? "" : "Cookie: SessionId=" + cookie + "\r\n") + "\r\n";
}
static std::string post(const char* path, const std::string& body, const std::string& cookie = {}) {
    return "POST " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nOrigin: http://127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + (cookie.empty() ? "" : "Cookie: SessionId=" + cookie + "\r\n") + "\r\n" + body;
}
static void token(const adm::Session& session, web::Reply* reply) {
    CHECK(id::EncodeHex(session.token.data(), session.token.size(), reply->session, sizeof(reply->session)));
    id::Fingerprint digest; CHECK(id::Sha256(reply->session, 64, digest)); CHECK(id::EncodeHex(digest.data(), digest.size(), reply->csrf, sizeof(reply->csrf)));
}
int main() {
    WSADATA data; CHECK(!WSAStartup(MAKEWORD(2, 2), &data));
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(probe != INVALID_SOCKET);
    sockaddr_in address{}; address.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    CHECK(!bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address))); int size = sizeof(address);
    CHECK(!getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size)); port = ntohs(address.sin_port); closesocket(probe);
    auto store = std::make_unique<adm::Store>(); const adm::Invoker console{{}, true}; id::Fingerprint administrator{}; administrator[0] = 1;
    CHECK(store->SetAdminWithPassword(console, administrator, "Fixture", 80, "password-123") == adm::Result::Ok);
    const auto copied = *store->FindAdmin(administrator);
    CHECK(adm::VerifyCredential(copied, "password-123")); CHECK(!adm::VerifyCredential(copied, "incorrect-password"));
    CHECK(!store->AdmitVerifiedCredential(copied, INT64_MAX));
    const auto* admitted = store->AdmitVerifiedCredential(copied, 1000); CHECK(admitted);
    web::Reply initial; token(*admitted, &initial); CHECK(store->SessionAdmin(initial.session, 1001));
    CHECK(!store->SessionAdmin(initial.session, 4600));
    CHECK(store->SetAdmin(console, administrator, "Fixture", 50) == adm::Result::Ok); CHECK(!store->AdmitVerifiedCredential(copied, 1000));
    CHECK(store->SetAdmin(console, administrator, "Fixture", 80) == adm::Result::Ok);
    CHECK(store->SetPassword(console, administrator, "replacement-123") == adm::Result::Ok);
    CHECK(!store->AdmitVerifiedCredential(copied, 1000)); CHECK(!store->SessionAdmin(initial.session, 1001));
    CHECK(store->SetPassword(console, administrator, "password-123") == adm::Result::Ok);
    const auto directory = std::filesystem::current_path() / ("webadmin-fixture-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory); { std::ofstream file(directory / "style.css"); file << "body{color:red}"; }
    web::Config config; config.port = port; CHECK(directory.wstring().size() < std::size(config.staticRoot)); std::wcscpy(config.staticRoot, directory.c_str());
    CHECK(KHZ_WebadminStart(config));
    for (unsigned i = 0; i != 500 && !KHZ_WebadminRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(KHZ_WebadminRunning());
    auto view = std::make_shared<web::View>(); view->serverSession = 1234; view->serverId = 42; std::strcpy(view->hostname, "<script>fixture</script>");
    std::strcpy(view->map, "mp_crash"); std::strcpy(view->gametype, "war"); view->administrators = store->Data().admins;
    view->players[0].connected = true; view->players[0].admission.identity[0] = 2; view->players[0].admission.generation = 7;
    std::strcpy(view->players[0].name, "<img>Player");
    view->players[1].connected = true; view->players[1].undercover = true; std::strcpy(view->players[1].name, "HiddenAdmin");
    KHZ_WebadminPublish(view);
    const auto status = exchange(get("/status")); CHECK(status.starts_with("HTTP/1.1 200")); CHECK(status.find("&lt;script&gt;") != std::string::npos);
    CHECK(status.find("<script>") == std::string::npos && status.find("HiddenAdmin") == std::string::npos);
    CHECK(exchange(get("/files/style.css")).find("body{color:red}") != std::string::npos);
    CHECK(exchange(get("/files/../secret")).starts_with("HTTP/1.1 404"));
    CHECK(exchange("GET /status HTTP/1.1\r\nHost: attacker.test\r\n\r\n").starts_with("HTTP/1.1 403"));
    std::string session, csrf;
    auto service = [&](web::Request request) {
        web::Reply reply;
        if (request.action == web::Action::Login) {
            CHECK(request.passwordVerified); const auto* accepted = store->AdmitVerifiedCredential(request.credential, 2000); CHECK(accepted);
            reply.authorized = true; reply.power = request.credential.power; std::strcpy(reply.name, request.credential.name); token(*accepted, &reply);
            session = reply.session; csrf = reply.csrf;
        } else {
            const auto* accepted = store->SessionAdmin(request.session, 2001);
            if (!accepted) reply.status = 401;
            else {
                reply.authorized = true; reply.power = accepted->power; std::strcpy(reply.name, accepted->name); std::strcpy(reply.csrf, csrf.c_str());
                if (request.action != web::Action::Page && !adm::ConstantTimeEqual(request.csrf, csrf)) reply.status = 403;
                else if (request.action == web::Action::Command) { CHECK(!std::strcmp(request.text, "ministatus")); std::strcpy(reply.text, "Actual bounded owner completion"); }
                else if (request.action == web::Action::Kick) { CHECK(request.targetSlot == 0 && request.target.generation == 7); }
            }
        }
        KHZ_WebadminComplete(request, reply);
    };
    auto ownedExchange = [&](const std::string& text) {
        auto result = std::async(std::launch::async, exchange, text);
        const auto deadline = GetTickCount64() + 5000;
        while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            CHECK(GetTickCount64() < deadline); web::Request request; if (KHZ_WebadminPoll(&request)) service(request);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return result.get();
    };
    const auto login = ownedExchange(post("/webadmin", "username=Fixture&password=password-123"));
    CHECK(login.starts_with("HTTP/1.1 200") && login.find("HttpOnly; SameSite=Strict") != std::string::npos && session.size() == 64);
    CHECK(ownedExchange(get("/webadmin", session)).find("Signed in as Fixture") != std::string::npos);
    CHECK(ownedExchange(post("/webadmin?action=sendcmd", "consolecommand=ministatus&csrf=" + csrf, session)).find("Actual bounded owner completion") != std::string::npos);
    CHECK(ownedExchange(post("/webadmin?action=sendcmd", "consolecommand=ministatus&csrf=" + std::string(64, '0'), session)).starts_with("HTTP/1.1 403"));
    auto pending = std::async(std::launch::async, exchange, post("/webadmin?action=sendcmd", "consolecommand=ministatus&csrf=" + csrf, session));
    web::Request abandoned; const auto deadline = GetTickCount64() + 5000;
    while (!KHZ_WebadminPoll(&abandoned)) { CHECK(GetTickCount64() < deadline); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    KHZ_WebadminErrorCleanup(); KHZ_WebadminComplete(abandoned, {});
    CHECK(pending.get().empty()); // Canceled owner work cannot return into a new admission.
    CHECK(exchange(get("/status")).starts_with("HTTP/1.1 200"));
    CHECK(store->RemoveAdmin(console, administrator) == adm::Result::Ok); CHECK(!store->AdmitVerifiedCredential(copied, 1000));
    KHZ_WebadminShutdown(); CHECK(!KHZ_WebadminRunning());
    std::filesystem::remove(directory / "style.css"); std::filesystem::remove(directory); WSACleanup();
    std::puts("webadmin worker/credential/lifecycle tests passed");
}
