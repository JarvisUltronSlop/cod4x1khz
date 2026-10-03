#pragma once
#include <cstdint>
struct msg_t;
struct netadr_t;

// Only the simulation owner reads its active UDP socket. Connectionless UI
// traffic and decoded lifecycle tails cross bounded queues to main.
void KHZ_ClientTransportPump();
bool KHZ_ClientTransportDeferPayload(const msg_t* decoded, bool hasHeader = false);
void KHZ_ClientTransportMainService();
void KHZ_ClientTransportReset();
void KHZ_ClientSimulationService();
void KHZ_ClientSimulationShutdown();
void KHZ_ClientTransportRegisterCheckpoint();
bool KHZ_ClientTransportBeginCheckpointFence(std::uint64_t fixture, unsigned timeoutMilliseconds = 5000);
bool KHZ_ClientTransportEndCheckpointFence(std::uint64_t fixture, unsigned timeoutMilliseconds = 5000);
bool KHZ_ClientTransportCheckpointFenced(std::uint64_t fixture, std::uint64_t session, std::uint64_t generation);

void CL_ParseServerPayload(int localClientNum, msg_t* decoded);
