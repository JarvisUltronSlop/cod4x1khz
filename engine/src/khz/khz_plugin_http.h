#pragma once
#include <cstdint>
struct ftRequest_t;
namespace khz::plugin_http {
inline constexpr unsigned MaxRequests=16,MaxPerPlugin=8,MaxPayload=8*1024*1024,MaxResponse=1024*1024,MaxBuffered=32*1024*1024;
// Init/Make/Shutdown are service/job work. Inputs are copied and charged to a
// fixed global budget. No endpoint is contacted unless a plugin supplies it.
bool Init();
ftRequest_t* Make(unsigned owner,std::uint64_t generation,const char* url,const char* method,const unsigned char* payload,int length,const char* headers);
int Poll(unsigned owner,std::uint64_t generation,ftRequest_t* request);
void Free(unsigned owner,std::uint64_t generation,ftRequest_t* request);
void Cancel(unsigned owner,std::uint64_t generation);
// Final unload only: all DLL callbacks/jobs have exited. Cancel keeps published
// response storage valid for their remaining reads; Discard releases it.
void Discard(unsigned owner,std::uint64_t generation);
void Shutdown();
struct Statistics {unsigned active=0,buffered=0;std::uint64_t submitted=0,completed=0,cancelled=0,rejected=0;};
Statistics Stats();
}
