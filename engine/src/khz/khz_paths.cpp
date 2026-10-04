#include "khz_paths.h"
#include <Windows.h>
#include <direct.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
char runtimeRoot[256]{};
char projectRoot[256]{};
char homeRoot[256]{};
bool benchmarkHome{};

std::vector<std::string> arguments(const char* commandLine) {
    std::vector<std::string> result;
    const char* p = commandLine ? commandLine : "";
    while (*p) {
        while (*p && *p <= ' ') ++p;
        if (!*p) break;
        std::string token;
        bool quoted = false;
        while (*p && (quoted || *p > ' ')) {
            if (*p == '"') quoted = !quoted;
            else token += *p;
            ++p;
        }
        result.push_back(std::move(token));
    }
    return result;
}

bool storePath(char* destination, const std::filesystem::path& path) {
    const auto value = path.string();
    if (value.size() >= 256) return false;
    std::memcpy(destination, value.c_str(), value.size() + 1);
    return true;
}

bool createFileOnce(const std::filesystem::path& path, const char* data) {
    HANDLE file = CreateFileA(path.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
    const auto length = static_cast<DWORD>(std::strlen(data));
    DWORD written = 0;
    const bool ok = WriteFile(file, data, length, &written, nullptr) && written == length;
    if (ok) FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) DeleteFileA(path.string().c_str());
    return ok;
}
}

bool KHZ_BootstrapPaths(const char* commandLine) {
    try {
        char module[MAX_PATH]{};
        const DWORD length = GetModuleFileNameA(nullptr, module, sizeof(module));
        if (!length || length >= sizeof(module)) return false;
        const std::filesystem::path executable(module);
        const auto directory = executable.parent_path();
        auto runtime = directory;
        if (!std::filesystem::is_directory(runtime / "zone") ||
            !std::filesystem::is_directory(runtime / "main")) runtime /= "runtime";
        if (!std::filesystem::is_directory(runtime / "zone") ||
            !std::filesystem::is_directory(runtime / "main")) {
            // Isolated architecture builds live below build/. Locate the same
            // project runtime without copying into deployed executable paths.
            auto ancestor = directory.parent_path();
            bool found = false;
            while (!ancestor.empty()) {
                const auto candidate = ancestor / "runtime";
                if (std::filesystem::is_directory(candidate / "zone") &&
                    std::filesystem::is_directory(candidate / "main")) {
                    runtime = candidate;
                    found = true;
                    break;
                }
                const auto parent = ancestor.parent_path();
                if (parent == ancestor) break;
                ancestor = parent;
            }
            if (!found) return false;
        }
        const auto project = runtime.parent_path();
        benchmarkHome = _stricmp(executable.filename().string().c_str(), "cod4x1khz_bench.exe") == 0;
        unsigned port = 28962;
        const auto args = arguments(commandLine);
        for (size_t i = 0; i + 2 < args.size(); ++i) {
            if (_stricmp(args[i].c_str(), "+set")) continue;
            if (!_stricmp(args[i + 1].c_str(), "khz_benchmark"))
                benchmarkHome = benchmarkHome || std::atoi(args[i + 2].c_str()) != 0;
            if (!_stricmp(args[i + 1].c_str(), "net_port")) {
                const int candidate = std::atoi(args[i + 2].c_str());
                if (candidate >= 1024 && candidate <= 65535) port = candidate;
            }
        }
#ifdef DEDICATED
        const auto home = benchmarkHome ? runtime / "benchmarks" / ("server-" + std::to_string(port))
                                        : runtime / "servers" / ("server-" + std::to_string(port));
#else
        const auto home = benchmarkHome ? runtime / "benchmarks" / ("client-" + std::to_string(port))
                                        : runtime / "interactive";
#endif
        if (!storePath(runtimeRoot, runtime) || !storePath(projectRoot, project) ||
            !storePath(homeRoot, home) || _chdir(runtimeRoot)) return false;
#ifndef DEDICATED
        const auto profiles = home / "players" / "profiles";
        const char* profileName = benchmarkHome ? "khz" : "cod4x1khz";
        std::filesystem::create_directories(profiles / profileName);
        // Only initial defaults differ from stock. Existing files are never overwritten.
        if (!createFileOnce(profiles / profileName / "config_mp.cfg",
            "seta com_maxfps \"0\"\r\nseta r_vsync \"0\"\r\n")) return false;
        const std::string active = std::string(profileName) + "\r\n";
        if (!createFileOnce(profiles / "active.txt", active.c_str())) return false;
#else
        std::filesystem::create_directories(home);
#endif
        return true;
    } catch (...) { return false; }
}

const char* KHZ_RuntimeRoot() { return runtimeRoot; }
const char* KHZ_ProjectRoot() { return projectRoot; }
const char* KHZ_HomeRoot() { return homeRoot; }
bool KHZ_IsBenchmarkHome() { return benchmarkHome; }
