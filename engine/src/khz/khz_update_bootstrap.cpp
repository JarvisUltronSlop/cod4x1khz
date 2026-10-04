#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "khz_update_bootstrap.h"
#include "khz_update_policy.h"
#include <TlHelp32.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <vector>

namespace khz::update {
namespace {
bool fail(std::string* reason,const char* text) { if(reason)*reason=text; return false; }
std::wstring normalized(const std::filesystem::path& path) {
    std::error_code error; auto value=std::filesystem::absolute(path,error).lexically_normal().wstring();
    if(error)return {}; std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return std::towlower(c);}); return value;
}
std::string hex(const content::Digest& hash) { std::string text(64,'0'); for(unsigned i=0;i<32;++i){text[2*i]="0123456789abcdef"[hash[i]>>4];text[2*i+1]="0123456789abcdef"[hash[i]&15];}return text; }
std::wstring mutexName(const std::filesystem::path& root) {
    const auto value=normalized(root); content::Digest hash{};
    if(value.empty()||!content::Sha256(value.data(),value.size()*sizeof(wchar_t),&hash))return {};
    const auto encoded=hex(hash); return L"Local\\cod4x1khz-update-"+std::wstring(encoded.begin(),encoded.end());
}
std::filesystem::path processImage(HANDLE process) {
    std::wstring value(32768,L'\0'); DWORD length=DWORD(value.size());
    if(!QueryFullProcessImageNameW(process,0,value.data(),&length))return {}; value.resize(length);return value;
}
std::uint64_t packed(FILETIME value){return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
std::uint64_t now(){FILETIME value{};GetSystemTimeAsFileTime(&value);return packed(value);}
bool creation(HANDLE process,std::uint64_t* value){FILETIME made{},exit{},kernel{},user{};if(!GetProcessTimes(process,&made,&exit,&kernel,&user))return false;*value=packed(made);return true;}
bool read(const std::filesystem::path& path,std::string* text,unsigned maximum=2048) {
    const DWORD flags=GetFileAttributesW(path.c_str());std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(flags==INVALID_FILE_ATTRIBUTES||(flags&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||error||size>maximum)return false;
    std::ifstream stream(path,std::ios::binary);text->resize(std::size_t(size));return bool(stream.read(text->data(),std::streamsize(size)));
}
bool number(std::string_view text,std::uint64_t* value){if(text.empty()||text.size()>20||(text.size()>1&&text.front()=='0'))return false;const auto p=std::from_chars(text.data(),text.data()+text.size(),*value);return p.ec==std::errc{}&&p.ptr==text.data()+text.size();}
bool journalHash(const std::filesystem::path& root,content::Digest* hash){std::filesystem::path path;return content::ConfinedPath(root,KHZ_UPDATE_DIRECTORY "/transaction.txt",&path)&&content::HashFile(path,hash);}
bool consumeTicket(const std::filesystem::path& root) {
    std::filesystem::path file;if(!content::ConfinedPath(root,KHZ_UPDATE_DIRECTORY "/launch-ticket.txt",&file))return false;
    std::string text;if(!read(file,&text))return false;std::string_view view(text);std::array<std::string_view,6> lines{};
    for(auto& line:lines){const auto end=view.find('\n');if(end==view.npos)return false;line=view.substr(0,end);view.remove_prefix(end+1);}
    if(!view.empty()||lines[0]!="KHZ_UPDATE_LAUNCH_1")return false;
    std::uint64_t pid=0,made=0,expires=0,currentCreation=0;content::Digest imageHash{},transactionHash{};
    const auto current=now();
    if(!number(lines[1],&pid)||pid!=GetCurrentProcessId()||!number(lines[2],&made)||!number(lines[3],&expires)
        ||current>expires||expires-current>600000000ull||!creation(GetCurrentProcess(),&currentCreation)||made!=currentCreation
        ||!IsInstalledExecutable(root,CurrentExecutable())||!content::HashFile(CurrentExecutable(),&imageHash)
        ||!journalHash(root,&transactionHash)||lines[4]!=hex(imageHash)||lines[5]!=hex(transactionHash))return false;
    return DeleteFileW(file.c_str())!=FALSE;
}
}
std::filesystem::path CurrentExecutable(){std::wstring value(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,value.data(),DWORD(value.size()));if(!length||length>=value.size())return {};value.resize(length);return value;}
bool IsInstalledExecutable(const std::filesystem::path& root,const std::filesystem::path& image){const auto actual=normalized(image);if(actual.empty())return false;for(const auto* path:InstalledPaths)if(actual==normalized(root/path))return true;
#ifdef _WIN64
    for(const auto* path:{"bin/x64/Release/cod4x1khz.exe","bin/x64/Release/cod4x1khz_server.exe"})if(actual==normalized(root/path))return true;
#endif
    return false;}
std::wstring QuoteArgument(std::wstring_view argument) {
    std::wstring result=L"\"";unsigned slashes=0;
    for(wchar_t c:argument){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){result.append(slashes*2+1,L'\\');result+=c;}else{result.append(slashes,L'\\');result+=c;}slashes=0;}
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
HANDLE AcquireProjectMutex(const std::filesystem::path& root,unsigned timeoutMs){const auto name=mutexName(root);if(name.empty())return nullptr;HANDLE mutex=CreateMutexW(nullptr,FALSE,name.c_str());if(!mutex)return nullptr;const DWORD wait=WaitForSingleObject(mutex,timeoutMs);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(mutex);return nullptr;}return mutex;}
bool WaitForStopped(const std::filesystem::path& root,DWORD ownerPid,unsigned timeoutMs,std::string* reason) {
    HANDLE owner=nullptr;
    if(ownerPid){owner=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,ownerPid);if(!owner&&GetLastError()!=ERROR_INVALID_PARAMETER)return fail(reason,"Cannot verify invoking game process");if(owner&&!IsInstalledExecutable(root,processImage(owner))){CloseHandle(owner);return fail(reason,"Invoking PID is not an installed project game");}}
    const auto deadline=GetTickCount64()+timeoutMs;unsigned clearPasses=0;
    while(GetTickCount64()<deadline){bool active=owner&&WaitForSingleObject(owner,0)==WAIT_TIMEOUT;
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE){if(owner)CloseHandle(owner);return fail(reason,"Cannot enumerate running project images");}
        PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
        if(!Process32FirstW(snapshot,&entry)){CloseHandle(snapshot);if(owner)CloseHandle(owner);return fail(reason,"Cannot inspect running project images");}
        do {if(entry.th32ProcessID==GetCurrentProcessId())continue;HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ProcessID);if(!process){bool matchingName=false;for(const auto* relative:InstalledPaths)matchingName|=_wcsicmp(entry.szExeFile,std::filesystem::path(relative).filename().c_str())==0;
                if(matchingName&&GetLastError()!=ERROR_INVALID_PARAMETER){CloseHandle(snapshot);if(owner)CloseHandle(owner);return fail(reason,"Cannot verify potentially running project image");}continue;}
            if(IsInstalledExecutable(root,processImage(process))&&WaitForSingleObject(process,0)==WAIT_TIMEOUT)active=true;CloseHandle(process);
        }while(Process32NextW(snapshot,&entry));CloseHandle(snapshot);
        if(!active){if(++clearPasses>=3){if(owner)CloseHandle(owner);return true;}}else clearPasses=0;Sleep(50);
    }
    if(owner)CloseHandle(owner);return fail(reason,"Timed out waiting for all project games to exit; no replacement attempted");
}
bool SpawnHelper(const std::filesystem::path& root,const char* action,std::uint64_t version,bool relaunch,std::string* reason,const content::Digest* expectedManifest) {
    if(!action||(std::string_view(action)!="recover"&&std::string_view(action)!="apply"&&std::string_view(action)!="rollback"))return fail(reason,"Invalid update action");
    if(std::string_view(action)=="apply"&&(!version||!expectedManifest))return fail(reason,"Apply requires the checked signed manifest digest and version");
    const auto image=CurrentExecutable();std::filesystem::path helper;
    if(!IsInstalledExecutable(root,image)||!content::ConfinedPath(root,HelperPath,&helper)||!VerifyBinary(helper))return fail(reason,"Verified project update helper is unavailable");
    std::wstring command=QuoteArgument(helper.wstring())+L" --action "+std::wstring(action,action+std::strlen(action))+L" --owner-pid "+std::to_wstring(GetCurrentProcessId());
    if(version)command+=L" --expected-version "+std::to_wstring(version);
    if(expectedManifest){const auto value=hex(*expectedManifest);command+=L" --expected-manifest "+std::wstring(value.begin(),value.end());}
    if(relaunch)command+=L" --relaunch-image "+QuoteArgument(image.wstring())+L" --relaunch-command "+QuoteArgument(GetCommandLineW());
    if(command.size()>=32767)return fail(reason,"Update helper command line exceeds Windows limit");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&process))return fail(reason,"Cannot launch stopped-process update helper");
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return true;
}
bool VerifiedRelaunch(const std::filesystem::path& root,const std::filesystem::path& image,const std::wstring& original,std::string* reason) {
    if(!IsInstalledExecutable(root,image)||!VerifyBinary(image)||original.empty()||original.size()>=32767)return fail(reason,"Relaunch image or command line is invalid");
    content::Digest imageHash{},transactionHash{};std::filesystem::path ticket;
    if(!content::HashFile(image,&imageHash)||!journalHash(root,&transactionHash)||!content::ConfinedPath(root,KHZ_UPDATE_DIRECTORY "/launch-ticket.txt",&ticket,true))return fail(reason,"Cannot prepare verified relaunch ticket");
    auto command=original;STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,root.c_str(),&startup,&process))return fail(reason,"Cannot create verified game restart");
    std::uint64_t made=0;const bool ready=creation(process.hProcess,&made);const auto text="KHZ_UPDATE_LAUNCH_1\n"+std::to_string(process.dwProcessId)+"\n"+std::to_string(made)+"\n"+std::to_string(now()+600000000ull)+"\n"+hex(imageHash)+"\n"+hex(transactionHash)+"\n";
    if(!ready||!content::AtomicWrite(ticket,text.data(),text.size())||ResumeThread(process.hThread)==DWORD(-1)){
        TerminateProcess(process.hProcess,1);DeleteFileW(ticket.c_str());CloseHandle(process.hThread);CloseHandle(process.hProcess);return fail(reason,"Cannot publish one-use verified restart ticket");}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return true;
}
BootstrapResult Bootstrap(const std::filesystem::path& root,std::string* reason) {
    if(!IsInstalledExecutable(root,CurrentExecutable())){fail(reason,"Game image is outside installed project paths");return BootstrapResult::Refused;}
    if(consumeTicket(root))return BootstrapResult::Proceed;
    HANDLE mutex=AcquireProjectMutex(root,0);if(!mutex){fail(reason,"Project updater is active; exiting this newly started image");return BootstrapResult::ExitForHelper;}ReleaseMutex(mutex);CloseHandle(mutex);
    std::filesystem::path probe;if(!content::ConfinedPath(root,".khz-bootstrap-root-check",&probe)){fail(reason,"Project root is invalid");return BootstrapResult::Refused;}
    const auto directory=root/StorageDirectory;const DWORD flags=GetFileAttributesW(directory.c_str());
    if(flags==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return BootstrapResult::Proceed;fail(reason,"Cannot inspect update directory");return BootstrapResult::Refused;}
    if(!content::ConfinedPath(root,KHZ_UPDATE_DIRECTORY "/transaction.txt",&probe)){fail(reason,"Update journal is outside confined project storage");return BootstrapResult::Refused;}
    if(GetFileAttributesW(probe.c_str())==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)return BootstrapResult::Proceed;fail(reason,"Cannot inspect update journal");return BootstrapResult::Refused;}
    Policy policy;
    if(!LoadPolicy(root,false,&policy,reason)||!Configured(policy)){if(reason&&reason->empty())*reason="Update journal exists but trusted production policy is unavailable";return BootstrapResult::Refused;}
    const auto recorded=InspectRecordedState(root,policy);
    if(recorded==RecordedState::Healthy||recorded==RecordedState::None)return BootstrapResult::Proceed;
    if(recorded==RecordedState::Invalid){fail(reason,"Update journal is invalid; refuse game launch");return BootstrapResult::Refused;}
    if(!SpawnHelper(root,"recover",0,true,reason))return BootstrapResult::Refused;
    return BootstrapResult::ExitForHelper;
}
}
