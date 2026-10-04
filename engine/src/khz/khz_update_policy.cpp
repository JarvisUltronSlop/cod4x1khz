#include "khz_update_policy.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>

namespace khz::update {
namespace {
bool failure(std::string* reason,const char* message) { if (reason) *reason=message; return false; }
bool read(const std::filesystem::path& file,unsigned maximum,std::string* result) {
    const DWORD flags=GetFileAttributesW(file.c_str()); std::error_code error;
    const auto bytes=std::filesystem::file_size(file,error);
    if (flags==INVALID_FILE_ATTRIBUTES || (flags&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) || error || bytes>maximum) return false;
    std::ifstream input(file,std::ios::binary); result->resize(std::size_t(bytes));
    return bool(input.read(result->data(),std::streamsize(bytes)));
}
template<std::size_t N> bool copy(std::string_view value,char (&output)[N]) {
    if (value.size()>=N) return false;
    for (unsigned char c:value) if (c<33 || c>126) return false;
    std::memcpy(output,value.data(),value.size()); output[value.size()]=0; return true;
}
bool decode(std::string_view text,PublicKey* result) {
    if (text.size()!=result->size()*2) return false;
    for (unsigned i=0;i<result->size();++i) {
        unsigned value=0;
        for (unsigned j=0;j<2;++j) {
            const char c=text[2*i+j];
            if (c>='0'&&c<='9') value=value*16+c-'0';
            else if (c>='a'&&c<='f') value=value*16+c-'a'+10;
            else return false;
        }
        (*result)[i]=std::uint8_t(value);
    }
    return true;
}
std::string encode(const PublicKey& key) {
    std::string result(key.size()*2,'0');
    for (unsigned i=0;i<key.size();++i) { result[2*i]="0123456789abcdef"[key[i]>>4]; result[2*i+1]="0123456789abcdef"[key[i]&15]; }
    return result;
}
}
bool ParsePolicy(std::string_view text,bool allowPrivateFixture,Policy* output,std::string* reason) {
    if (!output || text.empty() || text.size()>MaximumPolicyBytes || text.find('\0')!=text.npos) return failure(reason,"Policy is empty, overlong or contains NUL");
    std::array<std::string_view,6> lines{};
    for (auto& line:lines) {
        const auto end=text.find('\n'); if (end==text.npos) return failure(reason,"Policy requires six newline-terminated canonical fields");
        line=text.substr(0,end); if (!line.empty()&&line.back()=='\r') line.remove_suffix(1);
        if (line.find('\r')!=line.npos) return failure(reason,"Policy contains invalid carriage return");
        text.remove_prefix(end+1);
    }
    if (!text.empty() || lines[0]!="KHZ_UPDATE_POLICY_1" || lines[1]!="project=cod4x1khz") return failure(reason,"Policy project or field count is invalid");
    constexpr std::string_view names[]{"origin=","manifest_url=","trusted_key_cng_rsa2048=","private_http_fixture="};
    for (unsigned i=0;i<4;++i) { if (!lines[i+2].starts_with(names[i])) return failure(reason,"Policy fields must appear once in canonical order"); lines[i+2].remove_prefix(names[i].size()); }
    Policy policy;
    if (!copy(lines[2],policy.origin)||!copy(lines[3],policy.manifestUrl)
        || (lines[5]!="0"&&lines[5]!="1")) return failure(reason,"Policy endpoint or fixture flag is invalid");
    policy.privateHttpFixture=lines[5]=="1";
    if (policy.privateHttpFixture&&!allowPrivateFixture) return failure(reason,"Private HTTP update policy requires explicit fixture mode");
    const bool emptyKey=lines[4].empty();
    if (!emptyKey&&!decode(lines[4],&policy.trustedKey)) return failure(reason,"Policy key requires exact lowercase RSA2048 CNG public blob hex");
    const bool disabled=emptyKey&&!policy.origin[0]&&!policy.manifestUrl[0]&&!policy.privateHttpFixture;
    if (!disabled&&!Configured(policy)) return failure(reason,"Policy requires a valid trusted key and origin-pinned HTTPS manifest");
    *output=policy; if (reason) reason->clear(); return true;
}
bool LoadPolicy(const std::filesystem::path& root,bool allowPrivateFixture,Policy* result,std::string* reason) {
    std::filesystem::path file; std::string text;
    if (!result||!content::ConfinedPath(root,PolicyRelativePath,&file)||!read(file,MaximumPolicyBytes,&text)) return failure(reason,"Cannot read confined config/update-policy.txt");
    return ParsePolicy(text,allowPrivateFixture,result,reason);
}
bool FormatPolicy(const Policy& policy,std::string* output) {
    if (!output||!std::memchr(policy.origin,0,sizeof(policy.origin))||!std::memchr(policy.manifestUrl,0,sizeof(policy.manifestUrl))) return false;
    const bool emptyKey=std::all_of(policy.trustedKey.begin(),policy.trustedKey.end(),[](unsigned char c){return !c;});
    std::string text="KHZ_UPDATE_POLICY_1\nproject=cod4x1khz\norigin="+std::string(policy.origin)
        +"\nmanifest_url="+policy.manifestUrl+"\ntrusted_key_cng_rsa2048="+(emptyKey?"":encode(policy.trustedKey))
        +"\nprivate_http_fixture="+(policy.privateHttpFixture?"1\n":"0\n");
    Policy checked; if (!ParsePolicy(text,true,&checked)) return false;
    *output=std::move(text); return true;
}
bool ReadInstalledVersion(const std::filesystem::path& root,std::uint64_t* result) {
    std::filesystem::path file; std::string text;
    if (!result||!content::ConfinedPath(root,".khz-version-root-check",&file)) return false;
    const auto directory=root/StorageDirectory;
    const DWORD directoryFlags=GetFileAttributesW(directory.c_str());
    if (directoryFlags==INVALID_FILE_ATTRIBUTES) {
        const auto error=GetLastError();
        if (error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND) return false;
        *result=0; return true;
    }
    if (!(directoryFlags&FILE_ATTRIBUTE_DIRECTORY)||(directoryFlags&FILE_ATTRIBUTE_REPARSE_POINT)
        ||!content::ConfinedPath(root,KHZ_UPDATE_DIRECTORY "/version.txt",&file)) return false;
    const DWORD flags=GetFileAttributesW(file.c_str());
    if (flags==INVALID_FILE_ATTRIBUTES) { const auto error=GetLastError(); if (error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND) return false; *result=0; return true; }
    if (!read(file,64,&text)||text.empty()||text.back()!='\n') return false;
    text.pop_back(); if (text.empty()||text.size()>20||(text.size()>1&&text.front()=='0')) return false;
    std::uint64_t version=0; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),version);
    if (parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()) return false;
    *result=version; return true;
}
}
