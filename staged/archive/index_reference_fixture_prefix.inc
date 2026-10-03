// Only external diagnostic/Huffman readiness dependencies are fixtures.
// Actual native msg_t layout is emitted from msg_mp.h by the source extractor.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <msg_bits.hpp>
struct SnapshotInfo_s {};
struct DebugDvar { struct { bool enabled; } current{false}; };
DebugDvar debugDvar;
DebugDvar* msg_printEntityNums = &debugDvar;
constexpr int CON_CHANNEL_SERVER=0,CON_CHANNEL_SYSTEM=0;
inline bool SV_IsPacketDataNetworkData() { return false; }
inline void Com_Printf(int,const char*,...) { std::abort(); }
inline void MyAssertHandler(const char*,int,int,const char*,...) { std::abort(); }
inline void MSG_InitHuffman() { std::abort(); } // msgInit1: never called
inline int msgInit=1;
#define iassert(value) assert(value)
