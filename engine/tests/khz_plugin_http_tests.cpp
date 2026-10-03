#define KHZ_PLUGIN_HOST
#include "../../sdk/include/pinc.h"
#include "../../sdk/include/public_http_api.h"
#include <khz/khz_plugin_http.h>
#include <plugin_io_policy.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
namespace http=khz::plugin_http;
unsigned cases=0,failures=0;
void check(bool value,const char* name){++cases;if(!value){++failures;std::printf("FAIL: %s\n",name);}}
int complete(unsigned owner,std::uint64_t generation,ftRequest_t* request){
    const auto end=GetTickCount64()+6500;
    while(GetTickCount64()<end){const auto result=http::Poll(owner,generation,request);if(result)return result;Sleep(1);}return 0;
}
bool drained(){const auto end=GetTickCount64()+2000;while(GetTickCount64()<end){if(!http::Stats().active)return true;Sleep(2);}return false;}
void pure(){
    check(sizeof(msg_t)==(sizeof(void*)==4?40:48),"SDK msg layout matches rebuilt host pointer size");
    check(sizeof(httpPostVals_t)==32*2048,"public fixed form array");
    check(http::Init()&&http::Init(),"HTTP worker startup is idempotent");
    const char* badUrls[]={"", "ftp://127.0.0.1/x","HTTP://127.0.0.1/x","http://name@127.0.0.1/x","http://127.0.0.1/a b","http://127.0.0.1/a#fragment","http://127.0.0.1/a\\b","http://127.0.0.1/\r\nx"};
    for(const auto* value:badUrls)check(!http::Make(0,1,value,"GET",nullptr,0,""),"invalid URL refused before IO");
    check(!http::Make(16,1,"http://127.0.0.1:1/","GET",nullptr,0,""),"owner range bound");
    check(!http::Make(0,0,"http://127.0.0.1:1/","GET",nullptr,0,""),"zero generation refused");
    check(!http::Make(0,1,"http://127.0.0.1:1/","get",nullptr,0,""),"noncanonical method refused");
    check(!http::Make(0,1,"http://127.0.0.1:1/","GET",nullptr,-1,""),"negative length refused");
    check(!http::Make(0,1,"http://127.0.0.1:1/","POST",nullptr,1,""),"missing payload refused");
    const unsigned char byte=1;
    check(!http::Make(0,1,"http://127.0.0.1:1/","POST",&byte,http::MaxPayload+1,""),"payload capacity refused before copying");
    for(const char* headers:{"Host: other", "Content-Length: 10", "Transfer-Encoding: chunked", "X-Test: a\nInjected: b", "X-Test: a\r\nX-Test: b"})
        check(!http::Make(0,1,"http://127.0.0.1:1/","GET",nullptr,0,headers),"unsafe or duplicate headers refused");
    check(http::Poll(0,1,nullptr)==-1,"null poll rejected");
    ftRequest_t unrelated{};check(http::Poll(0,1,&unrelated)==-1,"foreign pointer rejected without dereference");
    http::Free(0,1,&unrelated);http::Cancel(0,2);
    check(!http::Make(0,1,"http://127.0.0.1:1/","GET",nullptr,0,""),"disabled generation tombstone blocks old request");
    check(!http::Make(0,2,"http://127.0.0.1:1/","GET",nullptr,0,""),"disabled current generation blocked");
    auto* next=http::Make(0,3,"http://127.0.0.1:1/","GET",nullptr,0,"");
    check(next!=nullptr,"higher reload generation may request");
    check(http::Poll(1,3,next)==-1&&http::Poll(0,4,next)==-1,"owner and generation both required");
    http::Free(1,3,next);check(complete(0,3,next)==-1,"connection refusal reported as transport error");
    http::Free(0,3,next);check(drained(),"freed request eventually drains without main IO");
    http::Shutdown();check(http::Stats().active==0&&http::Poll(0,3,next)==-1,"shutdown invalidates host objects");
}
void network(const char* base){
    check(http::Init(),"fixture startup");
    auto request=[&](const char* path,const char* method="GET",const unsigned char* payload=nullptr,int length=0,const char* headers=""){
        const auto target=std::string(base)+path;return http::Make(1,20,target.c_str(),method,payload,length,headers);
    };
    auto* get=request("/ok");check(get&&complete(1,20,get)==1,"real HTTP GET completes");
    check(get&&get->code==200&&get->complete&&get->contentLength==15&&get->extrecvmsg&&get->headerLength>4
        &&!std::memcmp(get->extrecvmsg->data+get->headerLength,"native-http-ok\n",15),"source-compatible body after headers");
    if(get){const auto* bytes=get->extrecvmsg?get->extrecvmsg->data:nullptr;check(complete(1,20,get)==1&&get->extrecvmsg&&get->extrecvmsg->data==bytes,"published response stays immutable");}
    http::Free(1,20,get);check(drained(),"GET reclaimed");
    std::vector<unsigned char> payload(8193);for(unsigned i=0;i<payload.size();++i)payload[i]=static_cast<unsigned char>(i*17);
    auto* post=request("/echo","POST",payload.data(),int(payload.size()),"Authorization: Bearer fixture\r\nContent-Type: application/octet-stream\r\nX-Native-Fixture: yes");
    payload.assign(payload.size(),0);check(post&&complete(1,20,post)==1,"POST executes once with explicit headers");
    bool copied=post&&post->code==200&&post->contentLength==8193&&post->extrecvmsg;
    if(copied)for(unsigned i=0;i<8193;++i)if(post->extrecvmsg->data[post->headerLength+i]!=static_cast<unsigned char>(i*17)){copied=false;break;}
    check(copied,"payload copied before worker IO and binary response preserved");http::Free(1,20,post);check(drained(),"POST reclaimed");
    auto* missing=request("/missing");check(missing&&complete(1,20,missing)==1&&missing->code==404,"HTTP errors expose complete response rather than transport failure");http::Free(1,20,missing);check(drained(),"404 reclaimed");
    auto* redirect=request("/redirect");check(redirect&&complete(1,20,redirect)==1&&redirect->code==302,"redirect returned without following or forwarding credentials");http::Free(1,20,redirect);check(drained(),"redirect reclaimed");
    auto* head=request("/head","HEAD");check(head&&complete(1,20,head)==1&&head->code==200&&head->contentLength==0,"HEAD permits announced resource length without receiving body");http::Free(1,20,head);check(drained(),"HEAD reclaimed");
    auto* chunked=request("/chunked");check(chunked&&complete(1,20,chunked)==1&&chunked->contentLength==15&&chunked->extrecvmsg
        &&!std::memcmp(chunked->extrecvmsg->data+chunked->headerLength,"native-http-ok\n",15),"chunked transfer decoded once by WinHTTP");http::Free(1,20,chunked);check(drained(),"chunked reclaimed");
    for(const auto* path:{"/oversize","/oversize-stream","/truncated","/huge-headers"}){
        auto* bad=request(path);check(bad&&complete(1,20,bad)==-1,"invalid or oversized HTTP response refused");http::Free(1,20,bad);check(drained(),"invalid response reclaimed");
    }
    const auto start=GetTickCount64();auto* slow=request("/slow");
    check(slow&&complete(1,20,slow)==-1&&GetTickCount64()-start<5600,"absolute five-second deadline includes transfer");http::Free(1,20,slow);check(drained(),"timed out asynchronous request fully drains");
    auto* cancel=request("/slow");Sleep(50);http::Cancel(1,20);
    check(cancel&&http::Poll(1,20,cancel)==-1,"plugin unload cancellation invalidates response");
    check(!request("/ok"),"cancelled generation cannot start another request");check(drained(),"cancellation drains private request resources");
    auto published=[&](std::uint64_t generation){return http::Make(3,generation,(std::string(base)+"/ok").c_str(),"GET",nullptr,0,"");};
    auto* held=published(30);check(held&&complete(3,30,held)==1,"published lifecycle fixture ready");
    const auto* heldBody=held&&held->extrecvmsg?held->extrecvmsg->data+held->headerLength:nullptr;
    http::Cancel(3,30);Sleep(50);
    check(heldBody&&http::Stats().active==1&&!std::memcmp(heldBody,"native-http-ok\n",15),"cancel preserves published bytes while pinned DLL job can still read");
    check(http::Poll(3,30,held)==-1&&!published(30),"cancel invalidates polls and new work without freeing published data");
    http::Free(3,30,held);check(drained(),"explicit free releases cancelled published response");
    held=published(31);check(held&&complete(3,31,held)==1,"reloaded generation published fixture ready");
    heldBody=held&&held->extrecvmsg?held->extrecvmsg->data+held->headerLength:nullptr;
    http::Cancel(3,31);Sleep(50);
    check(heldBody&&http::Stats().active==1&&!std::memcmp(heldBody,"native-http-ok\n",15),"second cancelled published response retained before final unload");
    http::Discard(3,31);check(drained(),"final discard after all DLL jobs exit reclaims retained response");
    std::array<ftRequest_t*,http::MaxPerPlugin> all{};
    for(auto& item:all){for(unsigned retry=0;retry<50&&!item;++retry){item=http::Make(2,21,(std::string(base)+"/slow").c_str(),"GET",nullptr,0,"");if(!item)Sleep(1);}}
    bool filled=true;for(auto* item:all)filled=filled&&item;check(filled,"eight bounded concurrent requests admitted");
    check(!http::Make(2,21,(std::string(base)+"/ok").c_str(),"GET",nullptr,0,""),"per-plugin ninth request rejected");
    check(http::Stats().active<=http::MaxRequests&&http::Stats().buffered<=http::MaxBuffered,"global buffers and slots bounded");
    http::Cancel(2,21);check(drained(),"bounded queued and running jobs cancel together");http::Shutdown();
}
}
int main(int argc,char** argv){pure();if(argc==2)network(argv[1]);std::printf("plugin HTTP: %u cases, %u failures%s\n",cases,failures,argc==2?" (loopback fixture)":" (no network fixture)");return failures?1:0;}
