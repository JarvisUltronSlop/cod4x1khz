#pragma once
#include "host_transport.hpp"
#include <filesystem>
#include <functional>

namespace khz::hosting {
class Controller {
  public:
    ~Controller();
    Controller() = default;
    Controller(const Controller &) = delete;
    enum class Phase { Idle, Starting, Loading, Running, Stopping, Failed };
    bool Start(const std::filesystem::path &root, const std::wstring &arguments,
               Packet configuration);
    void Poll();
    void Stop();
    void Close();
    bool Request(Packet value);
    Phase phase = Phase::Idle;
    Packet status{};
    std::string error;
    std::function<void(const Packet &)> onReply;
    std::function<bool(const Packet &)> initialReadyCheck;
    DWORD Pid() const { return pid_; }

  private:
    Channel channel_;
    HANDLE job_ = nullptr, process_ = nullptr;
    DWORD pid_ = 0;
    unsigned sent_ = 0, received_ = 0, pending_ = 0;
    ULONGLONG deadline_ = 0;
    Packet configuration_{};
    bool authenticated_ = false, initialReady_ = false;
    void Fail(const char *reason);
};
} // namespace khz::hosting
