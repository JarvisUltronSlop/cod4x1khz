#include <universal/q_shared.h>
#include <bgame/bg_local.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
static dvar_t stop{}, drag{}, inertia{}, angle{}, debug{}, melee{};
const dvar_t* stopspeed = &stop;
const dvar_t* friction = &drag;
const dvar_t* inertiaMax = &inertia;
const dvar_t* inertiaAngle = &angle;
const dvar_t* inertiaDebug = &debug;
const dvar_t* player_meleeChargeFriction = &melee;
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
void Com_Printf(int, const char*, ...) {}
double Jump_ReduceFriction(playerState_s*) { return 1.0; }
float Vec3Length(const float* v) { return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
float Vec3Normalize(float* v) { const float n = Vec3Length(v); if (n) for (int i=0;i<3;++i) v[i]/=n; return n; }
void Vec3Sub(const float* a,const float* b,float* out) { for(int i=0;i<3;++i) out[i]=a[i]-b[i]; }
float Vec3Dot(const float* a,const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
void Vec3Scale(const float* a,float s,float* out) { for(int i=0;i<3;++i) out[i]=a[i]*s; }
void Vec3Mad(const float* a,float s,const float* b,float* out) { for(int i=0;i<3;++i) out[i]=a[i]+s*b[i]; }
struct Run { float distance, speed; };
Run move(int milliseconds, float target) {
    playerState_s ps{}; pml_t pml{}; pml.walking = true; pml.msec = milliseconds;
    pml.frametime = milliseconds * .001f;
    const float right[]{0,1,0}; float distance = 0;
    for (int t=0;t<2000;t+=milliseconds) {
        PM_Friction(&ps, &pml);
        PM_Accelerate(&ps, &pml, right, target, 9.0f);
        distance += ps.velocity[1] * pml.frametime;
    }
    return {distance, ps.velocity[1]};
}
int main() {
    stop.current.value=100; drag.current.value=5.5f; inertia.current.value=50;
    // The production friction and acceleration functions must reach the
    // weapon's ADS speed from rest, with every right/left movement preserved.
    for (float target : {30.4f, 68.4f, 76.0f, 152.0f}) {
        const auto reference = move(50, target);
        for (int step : {1,2,5,10}) {
            const auto actual = move(step, target);
            CHECK(std::abs(actual.speed-target)<.01f);
            CHECK(actual.distance > reference.distance*.85f && actual.distance < reference.distance*1.15f);
            std::printf("%d ms target %.1f: distance %.3f speed %.3f\n", step,target,actual.distance,actual.speed);
        }
    }
    playerState_s ps{}; pml_t pml{}; pml.walking=true; pml.frametime=.001f;
    ps.velocity[1]=.9f; PM_Friction(&ps,&pml); CHECK(ps.velocity[1]>.34f);
    pml.frametime=.05f; ps.velocity[1]=.9f; PM_Friction(&ps,&pml); CHECK(ps.velocity[1]==0);
    // Releasing movement still brings ADS to a complete stop; no residual drift.
    pml.frametime=.001f; ps.velocity[1]=68.4f;
    for(int i=0;i<1000;++i) PM_Friction(&ps,&pml);
    CHECK(ps.velocity[1]==0);
}
