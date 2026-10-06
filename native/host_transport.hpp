#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <string>
#include "host_protocol.hpp"

namespace khz::hosting {
struct Channel {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED read{}, write{}, connect{};
    bool reading = false, writing = false, connecting = false, connected = false;
    bool peerVerified = false, failed = false;
    Packet incoming{};
    std::array<Packet, QueueCapacity> outgoing{};
    unsigned readIndex = 0, writeIndex = 0;
};
void Close(Channel &);
bool MakeEvents(Channel &);
bool OpenServer(Channel &, const wchar_t *name);
bool OpenClient(Channel &, const wchar_t *name, DWORD ownerPid);
bool PumpConnect(Channel &, DWORD childPid);
void PumpWrite(Channel &);
bool Receive(Channel &, Packet &);
bool Send(Channel &, const Packet &);
bool NewToken(char (&token)[33]);
bool SpawnOwned(const wchar_t *image, wchar_t *command, const wchar_t *cwd, HANDLE &job,
                HANDLE &process, DWORD &pid, std::string &error);
std::wstring Quote(std::wstring_view value);
} // namespace khz::hosting
