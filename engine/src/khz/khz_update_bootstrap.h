#pragma once
#include "khz_update.h"
#include <Windows.h>
#include <string>

namespace khz::update {
enum class BootstrapResult { Proceed, ExitForHelper, Refused };
// Call before engine initialization. ExitForHelper means return normally from
// WinMain so the separate helper can wait for this image to be unloaded.
BootstrapResult Bootstrap(const std::filesystem::path& projectRoot,std::string* reason=nullptr);
// Main control/lifecycle only. The caller must subsequently issue native quit.
bool SpawnHelper(const std::filesystem::path& projectRoot,const char* action,
    std::uint64_t expectedVersion,bool relaunch,std::string* reason=nullptr,
    const content::Digest* expectedManifest=nullptr);
std::filesystem::path CurrentExecutable();
bool IsInstalledExecutable(const std::filesystem::path& root,const std::filesystem::path& image);
std::wstring QuoteArgument(std::wstring_view argument);
HANDLE AcquireProjectMutex(const std::filesystem::path& root,unsigned timeoutMs);
bool WaitForStopped(const std::filesystem::path& root,DWORD ownerPid,unsigned timeoutMs,std::string* reason=nullptr);
// Used only after a verified apply/recovery. The one-use ticket is bound to the
// suspended child's PID, creation time, image hash and recovered journal hash.
bool VerifiedRelaunch(const std::filesystem::path& root,const std::filesystem::path& image,
    const std::wstring& commandLine,std::string* reason=nullptr);
}
