#include <universal/q_shared.h>
#include <ragdoll/ragdoll.h>
#include <cgame_mp/cg_local_mp.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool ragdollFirstInit;
BOOL ragdollInited;
RagdollDef ragdollDefs[2];
RagdollBody ragdollBodies[32];
const dvar_t *ragdoll_enable, *ragdoll_max_simulating;
cg_s cgArray[1];
unsigned captures;
BoneOrientation* Ragdoll_BodyBoneOrientations(RagdollBody* body) { return body->boneOrientations[body->curOrientationBuffer]; }
void Ragdoll_SnapshotAnimOrientations(RagdollBody*, BoneOrientation*) { ++captures; }
char Ragdoll_BodyNewState(RagdollBody* body, BodyState_t state) { body->state=state; return 1; }
bool Sys_IsMainThread() { return true; }
void Dvar_SetInt(dvar_t* dvar, int value) { dvar->current.integer=value; }
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
int main() {
    dvar_t enabled{}, maximum{};enabled.current.enabled=true;maximum.current.integer=4;
    ragdoll_enable=&enabled;ragdoll_max_simulating=&maximum;ragdollFirstInit=true;
    for(int cycle=0;cycle<3;++cycle) {
        Ragdoll_Shutdown();std::memset(ragdollBodies,0xc7,sizeof(ragdollBodies));
        for(auto& definition:ragdollDefs){definition.bound=true;definition.inUse=true;}
        Ragdoll_Init();
        const auto* bytes=reinterpret_cast<const unsigned char*>(ragdollBodies);
        for(unsigned n=0;n<sizeof(ragdollBodies);++n)assert(bytes[n]==0);
        for(const auto& definition:ragdollDefs)assert(!definition.bound&&!definition.inUse);
        assert(ragdollInited&&maximum.current.integer==8);
        ragdollBodies[31].references=1;Ragdoll_Init();assert(ragdollBodies[31].references==1);
    }
    Ragdoll_Shutdown();assert(!ragdollInited);
    assert(Ragdoll_PresentationMsec(1000)==0);
    assert(Ragdoll_PresentationMsec(1000)==0);
    assert(Ragdoll_PresentationMsec(1001)==1);
    assert(Ragdoll_PresentationMsec(1061)==60);
    assert(Ragdoll_PresentationMsec(1061)==0);
    assert(Ragdoll_PresentationMsec(10)==0);
    assert(Ragdoll_PresentationMsec(11)==1);
    RagdollBody body{};body.state=BS_VELOCITY_CAPTURE;
    cgArray[0].time=1000;Ragdoll_UpdateVelocityCapture(&body);
    assert(captures==1 && body.state==BS_VELOCITY_CAPTURE);
    // The owner advanced 60 ms while the renderer stalled; Com_Frame's old
    // duration is 1 ms. Use the time between the two actual animation poses.
    body.stateFrames=1;body.stateMsec=1;
    Ragdoll_UpdateVelocityCapture(&body);assert(captures==1);
    cgArray[0].time=1060;
    Ragdoll_UpdateVelocityCapture(&body);
    assert(captures==2 && body.velCaptureMsec==60 && body.state==BS_TUNNEL_TEST);
    std::printf("Native ragdoll reset: %zu-bit, all 32 bodies, three map lifecycles and pose-time velocity capture passed\n",sizeof(void*)*8);
}
