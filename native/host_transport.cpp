#include "host_transport.hpp"
#include <sddl.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <iterator>
namespace khz::hosting {
bool Send(Channel &channel, const Packet &value) {
    if (channel.failed || channel.writeIndex - channel.readIndex >= QueueCapacity)
        return false;
    channel.outgoing[channel.writeIndex++ % QueueCapacity] = value;
    return true;
}
bool OpenClient(Channel &channel, const wchar_t *name, DWORD ownerPid) {
    channel.pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
    if (channel.pipe == INVALID_HANDLE_VALUE || !MakeEvents(channel))
        return false;
    ULONG peer = 0;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!GetNamedPipeServerProcessId(channel.pipe, &peer) || peer != ownerPid ||
        !SetNamedPipeHandleState(channel.pipe, &mode, nullptr, nullptr))
        return false;
    channel.connected = channel.peerVerified = true;
    return true;
}
std::wstring Quote(std::wstring_view value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (auto ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
bool SpawnOwned(const wchar_t *image, wchar_t *command, const wchar_t *cwd, HANDLE &job,
                HANDLE &process, DWORD &pid, std::string &error) {
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = "Cannot create owned server job";
        if (job)
            CloseHandle(job);
        job = nullptr;
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(image, command, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, cwd, &startup, &child)) {
        error =
            "Cannot launch dedicated server (Windows error " + std::to_string(GetLastError()) + ")";
        CloseHandle(job);
        job = nullptr;
        return false;
    }
    if (!AssignProcessToJobObject(job, child.hProcess) ||
        ResumeThread(child.hThread) == DWORD(-1)) {
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        CloseHandle(job);
        job = nullptr;
        error = "Cannot attach dedicated server to its owner";
        return false;
    }
    CloseHandle(child.hThread);
    process = child.hProcess;
    pid = child.dwProcessId;
    return true;
}
void Close(Channel &channel) {
    if (channel.pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(channel.pipe, nullptr);
        // Teardown runs outside gameplay. Complete cancellation before reusing
        // the static OVERLAPPED records and their buffers for another session.
        DWORD ignored = 0;
        if (channel.reading)
            GetOverlappedResult(channel.pipe, &channel.read, &ignored, TRUE);
        if (channel.writing)
            GetOverlappedResult(channel.pipe, &channel.write, &ignored, TRUE);
        if (channel.connecting)
            GetOverlappedResult(channel.pipe, &channel.connect, &ignored, TRUE);
        CloseHandle(channel.pipe);
    }
    if (channel.read.hEvent)
        CloseHandle(channel.read.hEvent);
    if (channel.write.hEvent)
        CloseHandle(channel.write.hEvent);
    if (channel.connect.hEvent)
        CloseHandle(channel.connect.hEvent);
    channel = {};
}
bool MakeEvents(Channel &channel) {
    channel.read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    channel.write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    channel.connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return channel.read.hEvent && channel.write.hEvent && channel.connect.hEvent;
}
bool PumpConnect(Channel &channel, DWORD childPid) {
    if (channel.connecting) {
        DWORD bytes = 0;
        if (!GetOverlappedResult(channel.pipe, &channel.connect, &bytes, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE)
                return false;
            channel.failed = true;
            return false;
        }
        channel.connecting = false;
        channel.connected = true;
    }
    if (!channel.connected || channel.failed)
        return false;
    if (!channel.peerVerified) {
        ULONG peer = 0;
        if (!childPid || !GetNamedPipeClientProcessId(channel.pipe, &peer) || peer != childPid)
            channel.failed = true;
        else
            channel.peerVerified = true;
    }
    return channel.peerVerified && !channel.failed;
}
void PumpWrite(Channel &channel) {
    if (!channel.connected || channel.failed)
        return;
    DWORD bytes = 0;
    if (channel.writing) {
        if (!GetOverlappedResult(channel.pipe, &channel.write, &bytes, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE)
                return;
            channel.failed = true;
            return;
        }
        channel.writing = false;
        if (bytes != sizeof(Packet)) {
            channel.failed = true;
            return;
        }
        ++channel.readIndex;
    }
    if (channel.readIndex == channel.writeIndex)
        return;
    ResetEvent(channel.write.hEvent);
    const auto &value = channel.outgoing[channel.readIndex % QueueCapacity];
    if (WriteFile(channel.pipe, &value, sizeof(value), &bytes, &channel.write)) {
        if (bytes != sizeof(value))
            channel.failed = true;
        else
            ++channel.readIndex;
    } else if (GetLastError() == ERROR_IO_PENDING)
        channel.writing = true;
    else
        channel.failed = true;
}
bool Receive(Channel &channel, Packet &value) {
    if (!channel.connected || channel.failed)
        return false;
    DWORD bytes = 0;
    if (channel.reading) {
        if (!GetOverlappedResult(channel.pipe, &channel.read, &bytes, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE)
                return false;
            channel.failed = true;
            return false;
        }
        channel.reading = false;
    } else {
        ResetEvent(channel.read.hEvent);
        if (!ReadFile(channel.pipe, &channel.incoming, sizeof(Packet), &bytes, &channel.read)) {
            if (GetLastError() == ERROR_IO_PENDING) {
                channel.reading = true;
                return false;
            }
            channel.failed = true;
            return false;
        }
    }
    if (bytes != sizeof(Packet)) {
        channel.failed = true;
        return false;
    }
    value = channel.incoming;
    return true;
}
bool OpenServer(Channel &channel, const wchar_t *name) {
    HANDLE token = nullptr;
    alignas(TOKEN_USER) BYTE info[256]{};
    DWORD needed = 0;
    wchar_t *sid = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const bool gotUser =
        GetTokenInformation(token, TokenUser, info, sizeof(info), &needed) != FALSE;
    CloseHandle(token);
    if (!gotUser || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(info)->User.Sid, &sid))
        return false;
    wchar_t sddl[256];
    std::swprintf(sddl, std::size(sddl), L"D:P(A;;GA;;;%ls)(A;;GA;;;SY)", sid);
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor,
                                                              nullptr))
        return false;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    channel.pipe = CreateNamedPipeW(
        name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        sizeof(Packet) * 2, sizeof(Packet) * 2, 0, &attributes);
    LocalFree(descriptor);
    if (channel.pipe == INVALID_HANDLE_VALUE || !MakeEvents(channel))
        return false;
    if (ConnectNamedPipe(channel.pipe, &channel.connect))
        channel.connected = true;
    else if (GetLastError() == ERROR_IO_PENDING)
        channel.connecting = true;
    else if (GetLastError() == ERROR_PIPE_CONNECTED)
        channel.connected = true;
    else
        return false;
    return true;
}
bool NewToken(char (&token)[33]) {
    BYTE random[16];
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(random); ++i) {
        token[i * 2] = hex[random[i] >> 4];
        token[i * 2 + 1] = hex[random[i] & 15];
    }
    token[32] = 0;
    return true;
}
} // namespace khz::hosting
