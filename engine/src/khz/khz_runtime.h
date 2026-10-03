#pragma once
#include <cstdint>
#include "core.hpp"
struct msg_t;
struct client_t;
struct usercmd_s;
struct playerState_s;
struct clientSnapshot_t;
struct clSnapshot_t;
struct outPacket_t;
struct entityState_s;
struct clientState_s;
struct ClientArchiveData;
void KHZ_Init();
void KHZ_ClientRuntimeRegisterCheckpoint();
void KHZ_InstallCrashReporter();
bool KHZ_IsHeadless();
int KHZ_InputTime();
void KHZ_BenchmarkInput(usercmd_s* command);
void KHZ_PrintStats();
const char* KHZ_StatsJSON();
const char* KHZ_LogName();
void KHZ_WaitFrame();
bool KHZ_PresentDue();
void KHZ_PresentFrame();
bool KHZ_IsPresentationContext();
void KHZ_RecordGpuPresent(bool succeeded);
enum class KHZ_Timing { FrameWork, Prediction, Frontend, Backend, SwapFence, Present, RendererSync, Count };
void KHZ_RecordTiming(KHZ_Timing stage, std::uint64_t elapsed);
class KHZ_TimingScope {
public:
    explicit KHZ_TimingScope(KHZ_Timing stage) : stage_(stage), start_(khz::counter()) {}
    ~KHZ_TimingScope() { KHZ_RecordTiming(stage_, khz::counter() - start_); }
    KHZ_TimingScope(const KHZ_TimingScope&) = delete;
    KHZ_TimingScope& operator=(const KHZ_TimingScope&) = delete;
private:
    KHZ_Timing stage_;
    std::uint64_t start_;
};
void KHZ_ClientTick();
void KHZ_ResetServer();
void KHZ_ResetClient(client_t* client);
void KHZ_QueueCommand(client_t* client, const usercmd_s* command);
void KHZ_CommandIndex(client_t* client, unsigned index, unsigned count);
void KHZ_ApplyCommands();
void KHZ_BeginServerTick();
std::uint64_t KHZ_ServerTickNumber();
std::uint64_t KHZ_CurrentServerSession();
void KHZ_EndServerTick();
void KHZ_WriteClientHeader(msg_t* message);
khz::HeaderDisposition KHZ_ReadClientHeader(client_t* client, msg_t* message);
void KHZ_WriteServerHeader(client_t* client, msg_t* message);
bool KHZ_ReadServerHeader(msg_t* message);
void KHZ_CommitSnapshot(int messageNumber);
std::uint64_t KHZ_PredictionAck();
std::uint64_t KHZ_AcceptedClientSession();
void KHZ_ResetClientTransport();
// Rebase frontend startup after native same-map graphics/audio reload without
// clearing the accepted session or network/input histories.
void KHZ_ClientPresentationRestarted();
void KHZ_RecordPredictionCorrection(float distance);
void KHZ_RecordPredictionStep();
void KHZ_RunMovementReplay();
void KHZ_RunRewindProbe();
void KHZ_RunPhysicsProbe();
void KHZ_CaptureRewind();
bool KHZ_RewindClient(int client, int gameTime, float* origin, float* mins, float* maxs);
int KHZ_PredictionFirstCommand(int current);
const char* KHZ_StateJSON();
const char* KHZ_ContentFixtureReadJSON(const char* path);
void KHZ_RecordSnapshotSend(client_t* client, unsigned bytes);
clientSnapshot_t* KHZ_ServerFrames(client_t* client);
clSnapshot_t* KHZ_ClientSnapshots();
outPacket_t* KHZ_ClientPackets();
usercmd_s* KHZ_ClientCommands();
entityState_s* KHZ_ClientEntities();
clientState_s* KHZ_ClientStates();
ClientArchiveData* KHZ_ClientArchive();
unsigned KHZ_ScriptKey(std::uint64_t deadline);
unsigned KHZ_ScriptCurrentKey();
unsigned KHZ_ScriptDelayKey(double seconds);
unsigned KHZ_ScriptDelayKey(float seconds);
unsigned KHZ_ScriptFrameEndKey();
void KHZ_ScriptBeginFrameEnd();
void KHZ_ScriptEndFrameEnd();
bool KHZ_ScriptTestsEnabled();
const char* KHZ_ScriptProbeSource();
bool KHZ_ScriptIsCurrentKey(unsigned key);
unsigned KHZ_ScriptTakeDue();
void KHZ_ScriptRelease(unsigned key);
void KHZ_ScriptReset();
void KHZ_ScriptAdvance();
void KHZ_ScriptDecTime();
std::uint64_t KHZ_ScriptNow();
unsigned KHZ_BobCycle(playerState_s* ps, double increment);
void KHZ_AdvancePhase(int* phase, double increment);
void KHZ_RawInput(std::uintptr_t handle);
bool KHZ_RawMouse(int* dx, int* dy);
void KHZ_DiscardRawMouse();
bool KHZ_QueueKey(unsigned time, int key, int down);
void KHZ_PumpKeys();
