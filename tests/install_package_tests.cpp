#include "install_package.hpp"
#include <windows.h>
#include <cassert>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
static void put(const fs::path &file, const std::string &text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
    assert(out);
}
static std::string get(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--hold") {
        Sleep(30000);
        return 0;
    }
    using namespace khz::setup;
    auto root =
        fs::temp_directory_path() / ("khz-installer-test-" + std::to_string(GetCurrentProcessId()));
    assert(!fs::exists(root));
    fs::create_directory(root);
    auto source = root / "Steam game with spaces", destination = root / "Output with spaces";
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = sizeof(dos);
    IMAGE_FILE_HEADER header{};
    header.Machine = IMAGE_FILE_MACHINE_I386;
    DWORD signature = IMAGE_NT_SIGNATURE;
    fs::create_directory(source);
    std::ofstream exe(source / "iw3mp.exe", std::ios::binary);
    exe.write(reinterpret_cast<char *>(&dos), sizeof(dos));
    exe.write(reinterpret_cast<char *>(&signature), sizeof(signature));
    exe.write(reinterpret_cast<char *>(&header), sizeof(header));
    exe.close();
    put(source / "main/iw_00.iwd", "retail fixture");
    put(source / "localization.txt", "english\n");
    for (auto name :
         {"mp_killhouse.ff", "common_mp.ff", "code_post_gfx_mp.ff", "localized_common_mp.ff"})
        put(source / "zone/english" / name, "asset fixture");
    Plan plan;
    std::string error;
    assert(!Inspect(source, source / "overlap", plan, error));
    assert(!Inspect(source, root.root_path(), plan, error));
    put(destination / "unknown.txt", "must preserve");
    assert(!Inspect(source, destination, plan, error));
    fs::remove(destination / "unknown.txt");
    assert(Inspect(source, destination, plan, error));
    assert(!plan.repair && plan.assets.size() == 6);
    assert(!SafeRelative("../bad") && !SafeRelative("root/con.txt") &&
           !SafeRelative("root/trailing.") && !SafeRelative("C:/bad"));
    assert(SafeRelative("runtime/x64/cod4x1khz_server.exe"));
    std::atomic_bool cancelled = false;
    assert(
        !Install(plan, cancelled, [&](unsigned, const std::string &) { cancelled = true; }, error));
    assert(!fs::exists(destination) || fs::is_empty(destination));
    for (auto &entry : fs::directory_iterator(root))
        assert(!entry.path().filename().wstring().starts_with(L".cod4x1khz-install-"));
    cancelled = false;
    assert(Install(plan, cancelled, [](unsigned, const std::string &) {}, error));
    assert(fs::is_regular_file(destination / "cod4x1khz.exe") &&
           fs::is_regular_file(destination / "cod4x1khz_server_manager.exe"));
    assert(get(destination / "runtime/main/iw_00.iwd") == "retail fixture");
    auto clientHash = HashFile(destination / "cod4x1khz.exe");
    auto dependencyHash = HashFile(destination / "D3DX9_43.dll");
    fs::remove(destination / "D3DX9_43.dll");
    put(destination / "runtime/interactive/players/profiles/player/config_mp.cfg",
        "player choices");
    put(destination / "runtime/servers/stable/admins.dat", "admins");
    put(destination / "runtime/servers/stable/bans.dat", "bans");
    put(destination / "runtime/servers/stable/extra.cfg", "custom config");
    put(destination / "config/update-policy.txt", "custom update policy");
    put(destination / "cod4x1khz.exe", "corrupted project binary");
    assert(Inspect(source, destination, plan, error) && plan.repair && plan.assets.empty());
    assert(Install(plan, cancelled, [](unsigned, const std::string &) {}, error));
    assert(HashFile(destination / "cod4x1khz.exe") == clientHash);
    assert(HashFile(destination / "D3DX9_43.dll") == dependencyHash);
    assert(get(destination / "runtime/interactive/players/profiles/player/config_mp.cfg") ==
           "player choices");
    assert(get(destination / "runtime/servers/stable/admins.dat") == "admins" &&
           get(destination / "runtime/servers/stable/bans.dat") == "bans" &&
           get(destination / "runtime/servers/stable/extra.cfg") == "custom config");
    assert(get(destination / "config/update-policy.txt") == "custom update policy");
    put(destination / "cod4x1khz.exe", "rollback sentinel");
    auto locked = CreateFileW((destination / "cod4x1khz_server_manager.exe").c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!Install(plan, cancelled, [](unsigned, const std::string &) {}, error));
    CloseHandle(locked);
    assert(get(destination / "cod4x1khz.exe") == "rollback sentinel");
    assert(Install(plan, cancelled, [](unsigned, const std::string &) {}, error));
    wchar_t image[32768];
    assert(GetModuleFileNameW(nullptr, image, 32768));
    auto child = destination / "owned-fixture.exe";
    fs::copy_file(image, child);
    std::wstring command = L"\"" + child.wstring() + L"\" --hold";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    assert(CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, destination.c_str(), &startup, &process));
    assert(!Inspect(source, destination, plan, error) &&
           error.find("Close the installed") != error.npos);
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    fs::remove(child);
    assert(Inspect(source, destination, plan, error));
    auto manifest = get(destination / "cod4x1khz-install.txt");
    auto first = manifest.find("file=cod4x1khz.exe\t");
    assert(first != manifest.npos);
    auto end = manifest.find('\n', first);
    manifest.erase(first, end - first + 1);
    put(destination / "cod4x1khz-install.txt", manifest);
    assert(Inspect(source, destination, plan, error));
    assert(!Install(plan, cancelled, [](unsigned, const std::string &) {}, error));
    assert(error.find("unowned") != error.npos &&
           HashFile(destination / "cod4x1khz.exe") == clientHash);
    fs::remove(source / "zone/english/localized_common_mp.ff");
    assert(!Inspect(source, root / "another", plan, error));
    // Only this unique test-owned temporary directory is removed.
    assert(fs::weakly_canonical(root.parent_path()) ==
           fs::weakly_canonical(fs::temp_directory_path()));
    fs::remove_all(root);
    std::cout << "Installer spaces, assets, destination checks, cancellation, repair, profile "
                 "preservation and unowned-file protection passed\n";
}
