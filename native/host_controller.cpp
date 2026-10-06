#include "host_controller.hpp"
#include <cstring>
#include <cstdio>

namespace khz::hosting {
Controller::~Controller() { Close(); }
void Controller::Close() {
    hosting::Close(channel_);
    if (job_)
        CloseHandle(job_);
    if (process_)
        CloseHandle(process_);
    job_ = process_ = nullptr;
    pid_ = 0;
}
void Controller::Fail(const char *reason) {
    error = reason;
    Close();
    phase = Phase::Failed;
}
bool Controller::Start(const std::filesystem::path &root, const std::wstring &arguments,
                       Packet configuration) {
    if (process_) {
        error = "Stop the current server before starting another profile";
        return false;
    }
    Close();
    error.clear();
    sent_ = received_ = pending_ = 0;
    status = {};
    authenticated_ = initialReady_ = false;
    configuration_ = configuration;
    if (!NewToken(configuration_.token)) {
        Fail("Cannot generate host token");
        return false;
    }
    std::wstring pipe = PipePrefix + std::to_wstring(GetCurrentProcessId()) + L"-";
    pipe.append(configuration_.token, configuration_.token + 32);
    if (!OpenServer(channel_, pipe.c_str())) {
        Fail("Cannot open private server control channel");
        return false;
    }
    const auto image = root / "runtime/x64/cod4x1khz_server.exe";
    std::wstring token(configuration_.token, configuration_.token + 32);
    auto command = Quote(image.wstring()) + L" allowdupe +set dedicated 1 +set khz_host_parent " +
                   std::to_wstring(GetCurrentProcessId()) + L" +set khz_host_token " + token +
                   L" +set khz_host_pipe " + Quote(pipe) + L" " + arguments;
    if (!SpawnOwned(image.c_str(), command.data(), (root / "runtime").c_str(), job_, process_, pid_,
                    error)) {
        const auto reason = error;
        Fail(reason.c_str());
        return false;
    }
    phase = Phase::Starting;
    deadline_ = GetTickCount64() + ReadyTimeout;
    return true;
}
bool Controller::Request(Packet value) {
    if (!process_ || channel_.failed ||
        (phase != Phase::Running && value.message != HostMessage::Configure &&
         value.message != HostMessage::Stop))
        return false;
    value.sequence = ++sent_;
    value.pid = GetCurrentProcessId();
    std::memcpy(value.token, configuration_.token, sizeof(value.token));
    if (!Send(channel_, value))
        return false;
    if (value.message == HostMessage::Configure || value.message == HostMessage::Map ||
        value.message == HostMessage::Restart) {
        pending_ = value.sequence;
        phase = Phase::Loading;
        deadline_ = GetTickCount64() + ReadyTimeout;
    }
    return true;
}
void Controller::Stop() {
    if (!process_ || phase == Phase::Stopping)
        return;
    Packet value;
    value.message = HostMessage::Stop;
    Request(value);
    PumpWrite(channel_);
    phase = Phase::Stopping;
    deadline_ = GetTickCount64() + StopTimeout;
}
void Controller::Poll() {
    if (!process_)
        return;
    if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        if (phase == Phase::Stopping) {
            Close();
            phase = Phase::Idle;
        } else {
            const auto text =
                "Dedicated server exited (code " + std::to_string(code) + "); inspect its log";
            Fail(text.c_str());
        }
        return;
    }
    PumpConnect(channel_, pid_);
    PumpWrite(channel_);
    Packet reply;
    for (unsigned i = 0; i < 4 && Receive(channel_, reply); ++i) {
        if (reply.magic != WireMagic || reply.version != WireVersion || reply.protocol != 10002 ||
            reply.pid != pid_ || reply.sequence <= received_ ||
            std::memcmp(reply.token, configuration_.token, sizeof(reply.token)) ||
            !std::memchr(reply.output, 0, sizeof(reply.output)) ||
            !std::memchr(reply.map, 0, sizeof(reply.map)) ||
            !std::memchr(reply.error, 0, sizeof(reply.error)) ||
            !std::memchr(reply.gametype, 0, sizeof(reply.gametype)) || reply.playerCount > 16 ||
            reply.ruleCount > MaxRules) {
            Fail("Invalid owned-server reply");
            return;
        }
        for (unsigned rule = 0; rule < reply.ruleCount; ++rule)
            if (!std::memchr(reply.rules[rule].name, 0, sizeof(reply.rules[rule].name)) ||
                !std::memchr(reply.rules[rule].value, 0, sizeof(reply.rules[rule].value))) {
                Fail("Invalid rule status from owned server");
                return;
            }
        received_ = reply.sequence;
        for (unsigned player = 0; player < reply.playerCount; ++player) {
            const auto &row = reply.players[player];
            if (row.slot < 0 || row.slot >= 16 || row.bot > 1 || !row.generation ||
                !std::memchr(row.name, 0, sizeof(row.name)) ||
                !std::memchr(row.identity, 0, sizeof(row.identity))) {
                Fail("Invalid player status from owned server");
                return;
            }
        }
        if (reply.message == HostMessage::Hello && phase == Phase::Starting) {
            authenticated_ = true;
            configuration_.message = HostMessage::Configure;
            if (!Request(configuration_)) {
                Fail("Cannot configure owned server");
                return;
            }
        } else if (reply.message == HostMessage::Ready) {
            if (!authenticated_ || phase == Phase::Stopping || reply.port != configuration_.port ||
                (phase == Phase::Loading && reply.request != pending_) ||
                (!initialReady_ &&
                 (initialReadyCheck ? !initialReadyCheck(reply)
                                    : (std::strcmp(reply.map, configuration_.map) ||
                                       std::strcmp(reply.gametype, configuration_.gametype))))) {
                if (phase == Phase::Stopping)
                    continue;
                Fail("Readiness does not match the owned server");
                return;
            }
            initialReady_ = true;
            status = reply;
            phase = Phase::Running;
            pending_ = 0;
        } else if (reply.message == HostMessage::Failure) {
            Fail(*reply.error ? reply.error
                              : "Server rejected configuration or map load; inspect its log");
            return;
        } else if (reply.message == HostMessage::StatusResult)
            status = reply;
        if (onReply)
            onReply(reply);
    }
    PumpWrite(channel_);
    const auto now = GetTickCount64();
    if (phase == Phase::Stopping && now >= deadline_) {
        Close();
        phase = Phase::Idle;
    } else if (channel_.failed && phase != Phase::Stopping)
        Fail("Owned-server control channel closed");
    else if ((phase == Phase::Starting || phase == Phase::Loading) && now >= deadline_)
        Fail("Server startup timed out after 120 seconds");
}
} // namespace khz::hosting
