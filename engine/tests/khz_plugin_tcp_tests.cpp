#include <khz/khz_plugin_tcp.h>
#include <plugin_tcp_policy.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
namespace tcp=khz::plugin_tcp;
unsigned cases=0,failures=0;
void check(bool passed,const char* name){++cases;if(!passed){++failures;std::printf("FAIL: %s\n",name);}}
bool drained(){const auto end=GetTickCount64()+2000;while(GetTickCount64()<end){const auto stats=tcp::Stats();if(!stats.connected&&!stats.pending&&!stats.retiring)return true;Sleep(1);}return false;}
void pure(){
    tcp::policy::Endpoint endpoint;
    check(tcp::policy::Parse("localhost",&endpoint)&&endpoint.port==28960,"stock default port");
    check(tcp::policy::Parse("127.0.0.1:65535",&endpoint)&&endpoint.port==65535,"unsigned port maximum");
    check(tcp::policy::Parse("[::1]:1234",&endpoint)&&!std::strcmp(endpoint.host.data(),"::1")&&endpoint.port==1234,"bracket IPv6 syntax");
    check(tcp::policy::Parse("::1",&endpoint)&&endpoint.port==28960,"raw IPv6 default port");
    for(const char* invalid:{"","http://localhost:80","host:0","host:65536","host:-1","host:1x","host:","[::1","[::1]x","[::1]:","a b:80","a@b:80","host/path:80","a\nb:80"})
        check(!tcp::policy::Parse(invalid,&endpoint),"unsafe remote rejected before DNS");
    check(tcp::Init()&&tcp::Init(),"idempotent service startup");
    check(!tcp::Connect(16,1,0,"localhost:1"),"invalid owner");
    check(!tcp::Connect(0,0,0,"localhost:1"),"zero generation");
    check(!tcp::Connect(0,1,-1,"localhost:1")&&!tcp::Connect(0,1,4,"localhost:1"),"four-slot bounds");
    unsigned char byte=0;
    check(tcp::Receive(0,1,0,&byte,1)==-1&&tcp::Send(0,1,0,&byte,1)==-1,"closed IO reports -1");
    tcp::Cancel(0,5);
    check(!tcp::Connect(0,5,0,"localhost:1")&&!tcp::Connect(0,4,0,"localhost:1"),"retired current and prior generation rejected");
    tcp::Shutdown();check(!tcp::Stats().connected&&!tcp::Stats().pending,"shutdown state cleared");
}
void echo(unsigned owner,std::uint64_t generation,int connection){
    std::vector<unsigned char> payload(98317),received(payload.size());
    for(unsigned index=0;index<payload.size();++index)payload[index]=static_cast<unsigned char>(index*17+3);
    unsigned sent=0,read=0;bool bounded=true;const auto end=GetTickCount64()+2000;
    while((sent<payload.size()||read<received.size())&&GetTickCount64()<end){
        if(sent<payload.size()){
            const int count=tcp::Send(owner,generation,connection,payload.data()+sent,int(payload.size()-sent));
            if(count<0)break;bounded=bounded&&count<=int(tcp::MaxCallBytes);sent+=unsigned(count);
        }
        if(read<received.size()){
            const int count=tcp::Receive(owner,generation,connection,received.data()+read,int(received.size()-read));
            if(count<0)break;bounded=bounded&&count<=int(tcp::MaxCallBytes);read+=unsigned(count);
        }
        Sleep(1);
    }
    check(bounded&&sent==payload.size()&&read==payload.size()&&received==payload,"actual bounded partial binary send/receive exact echo");
}
void network(const char* ipv4,const char* hostname,const char* ipv6){
    check(tcp::Init(),"fixture startup");
    check(tcp::Connect(1,10,0,ipv4),"actual IPv4 connection");
    unsigned char byte=0;
    check(tcp::Receive(1,10,0,&byte,1)==0,"nonblocking empty receive");
    check(tcp::Receive(1,11,0,&byte,1)==-1&&tcp::Send(2,10,0,&byte,1)==-1,"owner and generation isolation");
    check(!tcp::Connect(1,10,0,ipv4),"occupied slot refused");
    echo(1,10,0);
    tcp::Close(1,10,0);check(drained(),"explicit close retires outside caller");
    check(tcp::Connect(1,10,0,hostname),"same-generation reconnect with actual hostname resolution");
    const char close[]="CLOSE\n";
    check(tcp::Send(1,10,0,close,int(sizeof(close)-1))==int(sizeof(close)-1),"fixture orderly close request");
    int status=0;const auto end=GetTickCount64()+2000;
    while(GetTickCount64()<end){status=tcp::Receive(1,10,0,&byte,1);if(status<0)break;Sleep(1);}
    check(status==-1&&drained(),"remote orderly closure reports -1 and retires");
    for(int connection=0;connection<4;++connection)check(tcp::Connect(3,100,connection,ipv4),"all four private module sockets connect");
    check(!tcp::Connect(3,100,4,ipv4),"fifth socket invalid");
    tcp::Cancel(3,100);
    check(tcp::Receive(3,100,0,&byte,1)==-1&&!tcp::Connect(3,100,0,ipv4),"cancel invalidates IO/new connects immediately");
    check(drained(),"generation cancel retires every socket");
    check(tcp::Connect(3,101,0,ipv4),"higher reload generation accepted");
    tcp::Close(3,100,0);echo(3,101,0);
    tcp::Discard(3,101);check(drained(),"final discard retires generation");
    if(ipv6){check(tcp::Connect(4,20,0,ipv6),"actual IPv6 loopback connection");echo(4,20,0);tcp::Close(4,20,0);check(drained(),"IPv6 close");}
#ifdef KHZ_PLUGIN_TCP_TESTS
    tcp::TestDelayResolution(1000);bool connected=true;
    std::thread pending([&]{connected=tcp::Connect(5,30,0,hostname);});
    const auto pendingEnd=GetTickCount64()+1000;while(!tcp::Stats().pending&&GetTickCount64()<pendingEnd)Sleep(1);
    check(tcp::Stats().pending==1,"controlled actual DNS-wait boundary pending");
    const auto cancelledAt=GetTickCount64();tcp::Cancel(5,30);pending.join();
    check(!connected&&GetTickCount64()-cancelledAt<500&&drained(),"cancel wakes pending connect and forbids late completion");
    tcp::TestDelayResolution(5100);const auto began=GetTickCount64();
    check(!tcp::Connect(6,40,0,hostname),"absolute DNS/connect deadline rejects late success");
    const auto duration=GetTickCount64()-began;
    check(duration>=4900&&duration<=5600&&drained(),"five-second deadline bounded including DNS boundary");
    tcp::TestDelayResolution(0);
#endif
    check(tcp::Connect(7,50,0,ipv4),"active socket before host shutdown");
    tcp::Shutdown();check(!tcp::Stats().connected&&tcp::Receive(7,50,0,&byte,1)==-1,"stopped host invalidates all sockets");
    check(tcp::Init(),"lifecycle may restart after all retired");tcp::Shutdown();
}
}
int main(int argc,char** argv){
    pure();if(argc>=3)network(argv[1],argv[2],argc>=4?argv[3]:nullptr);
    std::printf("KHZ_PLUGIN_TCP_TEST {\"cases\":%u,\"failures\":%u,\"network_fixture\":%s,\"ipv6_fixture\":%s}\n",cases,failures,argc>=3?"true":"false",argc>=4?"true":"false");
    return failures?1:0;
}
