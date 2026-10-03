#pragma once
#include <cstdint>

namespace khz::plugin_tcp {
inline constexpr unsigned MaxOwners=16,ConnectionsPerOwner=4,MaxCallBytes=65536,DeadlineMilliseconds=5000;
bool Init();
// Host permits this only on a pinned plugin job/thread, never the authoritative
// owner. Success means an actual connected nonblocking socket, not a queued job.
bool Connect(unsigned owner,std::uint64_t generation,int connection,const char* remote);
int Receive(unsigned owner,std::uint64_t generation,int connection,void* output,int capacity);
int Send(unsigned owner,std::uint64_t generation,int connection,const void* input,int length);
void Close(unsigned owner,std::uint64_t generation,int connection);
// Cancel retires the generation and closes asynchronously. Discard is called
// after all plugin jobs/callbacks leave; no worker reads the plugin registry.
void Cancel(unsigned owner,std::uint64_t generation);
void Discard(unsigned owner,std::uint64_t generation);
// Lifecycle barrier: host first rejects new SDK calls and waits for every
// pinned job/thread/callback. Shutdown is never concurrent with ANY backend
// caller (including Stats). Cancellation alone may race an already validated
// nonblocking IO call; that call may return its actual in-flight byte count.
void Shutdown();
struct Statistics {
    unsigned connected=0,pending=0,retiring=0;
    std::uint64_t submitted=0,completed=0,failed=0,cancelled=0,rejected=0,bytesSent=0,bytesReceived=0;
    bool stopping=false;
};
Statistics Stats();
#ifdef KHZ_PLUGIN_TCP_TESTS
// Isolated target only: controlled delay at the real async-DNS wait boundary.
void TestDelayResolution(unsigned milliseconds);
#endif
}
