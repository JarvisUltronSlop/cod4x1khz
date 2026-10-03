#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <khz/khz_update_bootstrap.h>
#include <khz/khz_update_policy.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>

namespace {
namespace update=khz::update;
namespace content=khz::content;
struct Options {
    std::string action;
    DWORD ownerPid=0;
    std::uint64_t expectedVersion=0;
    std::string expectedManifest;
    bool privateFixture=false;
    std::filesystem::path fixtureRoot,relaunchImage;
    std::wstring relaunchCommand;
};
bool decimal(std::wstring_view value,std::uint64_t* result){if(value.empty()||value.size()>20||(value.size()>1&&value.front()==L'0'))return false;std::string text;for(auto c:value){if(c<L'0'||c>L'9')return false;text+=char(c);}const auto p=std::from_chars(text.data(),text.data()+text.size(),*result);return p.ec==std::errc{}&&p.ptr==text.data()+text.size();}
bool parse(int argc,wchar_t** argv,Options* output){
    Options result;unsigned seen=0;
    for(int i=1;i<argc;++i){const std::wstring_view key=argv[i];unsigned bit=0;
        if(key==L"--private-http-fixture"){bit=64;if(seen&bit)return false;seen|=bit;result.privateFixture=true;continue;}
        if(i+1>=argc)return false;const std::wstring_view value=argv[++i];
        if(key==L"--action"){bit=1;for(auto c:value){if(c>127)return false;result.action+=char(c);}}
        else if(key==L"--owner-pid"){bit=2;std::uint64_t number=0;if(!decimal(value,&number)||!number||number>MAXDWORD)return false;result.ownerPid=DWORD(number);}
        else if(key==L"--expected-version"){bit=4;if(!decimal(value,&result.expectedVersion)||!result.expectedVersion)return false;}
        else if(key==L"--expected-manifest"){bit=128;if(value.size()!=64)return false;for(auto c:value){if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;result.expectedManifest+=char(c);}}
        else if(key==L"--fixture-root"){bit=8;if(value.empty()||value.size()>30000)return false;result.fixtureRoot=value;}
        else if(key==L"--relaunch-image"){bit=16;if(value.empty()||value.size()>30000)return false;result.relaunchImage=value;}
        else if(key==L"--relaunch-command"){bit=32;if(value.empty()||value.size()>30000)return false;result.relaunchCommand=value;}
        else return false;
        if(seen&bit)return false;seen|=bit;
    }
    if(result.action!="check"&&result.action!="apply"&&result.action!="recover"&&result.action!="rollback")return false;
    if((result.action=="apply")!=bool(result.expectedVersion)||(result.action=="apply")!=bool(result.expectedManifest.size())||result.relaunchImage.empty()!=result.relaunchCommand.empty())return false;
    if(!result.fixtureRoot.empty()&&(result.ownerPid||!result.relaunchImage.empty()))return false;
    if(result.privateFixture&&result.fixtureRoot.empty())return false;
    *output=std::move(result);return true;
}
std::wstring normalized(const std::filesystem::path& file){std::error_code error;auto text=std::filesystem::absolute(file,error).lexically_normal().wstring();if(error)return {};std::transform(text.begin(),text.end(),text.begin(),[](wchar_t c){return std::towlower(c);});return text;}
bool rootFor(const Options& options,std::filesystem::path* root){
    const auto image=update::CurrentExecutable();
    if(options.fixtureRoot.empty()){
        const auto candidate=image.parent_path().parent_path();std::filesystem::path helper;
        if(!content::ConfinedPath(candidate,"runtime/cod4x1khz_update.exe",&helper)||normalized(image)!=normalized(helper))return false;*root=candidate;return true;
    }
    auto project=image.parent_path();bool found=false;
    for(unsigned i=0;i<8&&!project.empty();++i){std::filesystem::path marker;if(content::ConfinedPath(project,"config/update-policy.txt",&marker)&&GetFileAttributesW(marker.c_str())!=INVALID_FILE_ATTRIBUTES){found=true;break;}const auto parent=project.parent_path();if(parent==project)break;project=parent;}
    if(!found)return false;
    const auto prefix=normalized(project/L"results")+L"\\";const auto candidate=normalized(options.fixtureRoot);
    if(candidate.size()<=prefix.size()||candidate.compare(0,prefix.size(),prefix)!=0)return false;
    std::filesystem::path probe;if(!content::ConfinedPath(options.fixtureRoot,".khz-update-fixture-check",&probe))return false;
    *root=std::filesystem::absolute(options.fixtureRoot).lexically_normal();return true;
}
std::string jsonString(std::string_view text){std::string result="\"";for(unsigned char c:text){if(c=='\"'||c=='\\'){result+='\\';result+=char(c);}else if(c>=32&&c<127)result+=char(c);else result+='?';}return result+'\"';}
std::string manifestHash(const update::Manifest& manifest){std::string body;content::Digest hash{};if(!update::CanonicalBody(manifest,&body)||!content::Sha256(body.data(),body.size(),&hash))return {};std::string result(64,'0');for(unsigned i=0;i<32;++i){result[2*i]="0123456789abcdef"[hash[i]>>4];result[2*i+1]="0123456789abcdef"[hash[i]&15];}return result;}
int report(const std::filesystem::path& root,const Options& options,bool success,const std::string& detail,const update::Result& result={},std::uint64_t version=0,const std::string& selectedHash={}){
    const auto text="{\"format\":1,\"action\":"+jsonString(options.action)+",\"success\":"+(success?"true":"false")+",\"version\":"+std::to_string(version)
        +",\"manifest_sha256\":"+jsonString(selectedHash)+",\"applied\":"+(result.applied?"true":"false")+",\"rolled_back\":"+(result.rolledBack?"true":"false")+",\"recoverable\":"+(result.recoverable?"true":"false")+",\"detail\":"+jsonString(detail)+"}\n";
    std::filesystem::path file;if(!root.empty()&&content::ConfinedPath(root,"updates/last-result.json",&file,true))content::AtomicWrite(file,text.data(),text.size());
    std::printf("%s",text.c_str());return success?0:1;
}
bool download(const std::filesystem::path& root,const update::Policy& policy,const update::Manifest& manifest,std::array<std::filesystem::path,2>* sources,std::string* reason){
    std::filesystem::path staging,check;if(!content::ConfinedPath(root,"updates/incoming/staging/root-check",&check,true)){*reason="Cannot confine update staging";return false;}
    staging=check.parent_path();
    if(!content::Start(root,staging)){*reason="Cannot start private update download worker";return false;}
    bool okay=true;for(unsigned i=0;i<2;++i){const auto& file=manifest.files[i];content::Request request;request.id=i+1;request.session=1;request.generation=manifest.version;request.expectedBytes=file.bytes;request.sha256=file.sha256;
        if(file.bytes>128ull*1024*1024){okay=false;break;}std::snprintf(request.url,sizeof(request.url),"%s",file.url);std::snprintf(request.origin,sizeof(request.origin),"%s",policy.origin);okay&=content::SubmitVerifiedBlob(request);}
    unsigned received=0;const auto deadline=GetTickCount64()+30000;
    while(okay&&received!=3&&GetTickCount64()<deadline){content::Completion completion;if(!content::Poll(&completion)){Sleep(5);continue;}
        if(completion.request.id<1||completion.request.id>2||completion.status!=content::Status::Staged){okay=false;break;}const unsigned index=unsigned(completion.request.id-1);
        if((received&(1u<<index))||!content::StagedPath(completion,1,manifest.version,&(*sources)[index])){okay=false;break;}received|=1u<<index;
    }
    if(received!=3)okay=false;
    if(!okay){content::Cancel(1,manifest.version);content::Stop();*reason="Signed update download failed, timed out or changed after staging";}
    // Keep the successful staging owner alive through Apply. StagedPath has
    // verified each completion, and Apply independently verifies both sources.
    return okay;
}
}
int wmain(int argc,wchar_t** argv){
    Options options;if(!parse(argc,argv,&options)){std::fprintf(stderr,"Invalid bounded update-helper arguments\n");return 2;}
    std::filesystem::path root;if(!rootFor(options,&root))return report({},options,false,"Helper location or fixture root is outside this project");
    update::Policy policy;std::string reason;
    if(!update::LoadPolicy(root,options.privateFixture,&policy,&reason)||!update::Configured(policy))return report(root,options,false,reason.empty()?"Updates are disabled; configure an explicit trusted project policy":reason);
    std::uint64_t installed=0;if(!update::ReadInstalledVersion(root,&installed))return report(root,options,false,"Installed version record is invalid");
    if(options.action=="check"){update::Manifest manifest;if(!update::FetchManifest(policy,installed,&manifest))return report(root,options,false,"Manifest fetch, signature, origin, project or version verification failed");return report(root,options,true,"Trusted project update is available",{},manifest.version,manifestHash(manifest));}
    HANDLE mutex=update::AcquireProjectMutex(root,30000);if(!mutex)return report(root,options,false,"Another updater owns this project");
    struct Lock{HANDLE value;~Lock(){ReleaseMutex(value);CloseHandle(value);}}lock{mutex};
    if(!options.relaunchImage.empty()&&!update::IsInstalledExecutable(root,options.relaunchImage))return report(root,options,false,"Relaunch image is outside installed project paths");
    if(!update::WaitForStopped(root,options.ownerPid,30000,&reason))return report(root,options,false,reason);
    update::Result result;
    if(options.action=="apply"){
        // Always re-fetch after the owning game has stopped. A prior console
        // check only pins the selected version and never authorizes new bytes.
        update::Manifest manifest;if(!update::ReadInstalledVersion(root,&installed)||!update::FetchManifest(policy,installed,&manifest)||manifest.version!=options.expectedVersion||manifestHash(manifest)!=options.expectedManifest)return report(root,options,false,"Selected signed manifest changed or is no longer newer");
        std::array<std::filesystem::path,2> sources;
        if(!download(root,policy,manifest,&sources,&reason))return report(root,options,false,reason);
        result=update::Apply(root,manifest,policy,sources,installed);content::Stop();
    }else if(options.action=="recover")result=update::Recover(root,policy);
    else result=update::Rollback(root,policy);
    const bool success=options.action=="apply"?result.applied:result.applied||result.rolledBack||(!result.recoverable&&std::strcmp(result.detail,"No update transaction")==0);
    if(!success)return report(root,options,false,result.detail,result);
    if(!options.relaunchImage.empty()&&!update::VerifiedRelaunch(root,options.relaunchImage,options.relaunchCommand,&reason))return report(root,options,false,reason,result);
    return report(root,options,true,result.detail,result);
}
