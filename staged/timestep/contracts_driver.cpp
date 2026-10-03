// Dependency fixtures for extracted native function bodies. No BSP/VM/ABI proof.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#define iassert(x) do { if (!(x)) std::abort(); } while (0)
#define IS_NAN(x) (!std::isfinite(float(x)))
#define LODWORD(x) (*reinterpret_cast<std::uint32_t*>(&(x)))
namespace dependency {
constexpr int ENTITYNUM_NONE=1023, FL_GROUND_ENT=0x100, ERR_DROP=1, BUTTON_ATTACK=1;
constexpr float EQUAL_EPSILON=0.001f;
enum { TR_STATIONARY, TR_INTERPOLATE, TR_PHYSICS, TR_RAGDOLL_INTERPOLATE,
 TR_LINEAR, TR_FIRST_RAGDOLL, TR_LINEAR_STOP, TR_SINE, TR_GRAVITY,
 TR_RAGDOLL_GRAVITY, TR_ACCELERATE, TR_DECELERATE };
enum { MISSILESTAGE_ASCENT, MISSILESTAGE_DESCENT };
struct trajectory_t { int trType=TR_STATIONARY,trTime=0,trDuration=0; float trBase[3]{},trDelta[3]{}; };
struct gclient_s { int buttons=0; };
struct scr_vehicle_s { struct { int fireTime=0; } turret; };
struct Owner { int value=-1; bool isDefined() const {return value>=0;} int entnum() const {return value;} };
struct gentity_s {
 struct { struct { trajectory_t pos; } lerp; int groundEntityNum=ENTITYNUM_NONE,weapon=0; } s;
 struct { float currentOrigin[3]{},mins[3]{},maxs[3]{}; int contents=0; Owner ownerNum; bool inuse=true; } r;
 struct { int stage=MISSILESTAGE_ASCENT; } missile;
 int spawnflags=0,flags=0; void* tagInfo=nullptr; int nextthink=0;
 scr_vehicle_s* scr_vehicle=nullptr; gclient_s* client=nullptr;
};
struct trace_t { float fraction=1; float normal[3]{0,0,1}; bool startsolid=false; int hit=42; };
gentity_s g_entities[1024];
struct {int time=0,frametime=1; gentity_s* gentities=g_entities;} level;
struct dvar_t {struct {float value=0;} current;};
dvar_t climb,descent,climbCap,descentCap,drag;
dvar_t *missileJavAccelClimb=&climb,*missileJavAccelDescend=&descent,
 *missileJavSpeedLimitClimb=&climbCap,*missileJavSpeedLimitDescend=&descentCap,*missileJavTurnDecel=&drag;
struct Observation {std::vector<trace_t> traces; unsigned used=0; std::vector<std::array<float,3>> starts,ends;
 std::vector<int> masks,owners; std::string order; bool sky=false,freeOnThink=false;
 unsigned heliCalls=0,shots=0; double integratedSeconds=0; float turn=0; int gunner=0; } observed;
unsigned checks=0;
void check(bool value,const char* text,int line) {++checks;if(!value){std::fprintf(stderr,"contract:%d:%s\n",line,text);std::abort();}}
#define CHECK(x) dependency::check(bool(x),#x,__LINE__)
void near(float a,float b,float tolerance=0.002f){CHECK(std::fabs(a-b)<=tolerance);}
void MyAssertHandler(const char*,int,int,const char*,...){std::abort();}
void Com_Error(int,const char*,...){std::abort();}
float Vec3LengthSq(const float* v){return v[0]*v[0]+v[1]*v[1]+v[2]*v[2];}
float Vec3Length(const float* v){return std::sqrt(Vec3LengthSq(v));}
float Vec3NormalizeTo(const float* v,float* out){const float n=Vec3Length(v);for(int i=0;i<3;++i)out[i]=n?v[i]/n:0;return n;}
void Vec3Sub(const float* a,const float* b,float* o){for(int i=0;i<3;++i)o[i]=a[i]-b[i];}
void Vec3Scale(const float* a,float f,float* o){for(int i=0;i<3;++i)o[i]=a[i]*f;}
float Vec3Dot(const float* a,const float* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
void Vec3Mad(const float* a,float f,const float* b,float* o){for(int i=0;i<3;++i)o[i]=a[i]+f*b[i];}
void Vec3Lerp(const float* a,const float* b,float f,float* o){for(int i=0;i<3;++i)o[i]=a[i]+f*(b[i]-a[i]);}
int G_ItemClipMask(gentity_s*){return 0x123400;}
void G_TraceCapsule(trace_t* out,const float* a,const float*,const float*,const float* b,int owner,int mask){
 observed.order+='T';observed.starts.push_back({a[0],a[1],a[2]});observed.ends.push_back({b[0],b[1],b[2]});
 observed.owners.push_back(owner);observed.masks.push_back(mask);
 *out=observed.used<observed.traces.size()?observed.traces[observed.used]:trace_t{};++observed.used;
}
void SV_LinkEntity(gentity_s*){observed.order+='L';}
void G_RunThink(gentity_s* e){observed.order+='K';if(observed.freeOnThink)e->r.inuse=false;}
void G_FreeEntity(gentity_s* e){observed.order+='F';e->r.inuse=false;}
void G_OrientItemToGround(gentity_s*,trace_t*){observed.order+='O';}
int SV_PointContents(const float*,int,int){return observed.sky?1:0;}
int Trace_GetEntityHitId(const trace_t* tr){return tr->hit;}
int VehicleEntGunner(gentity_s*){return observed.gunner;}
void FireTurret(gentity_s* e,gentity_s*){++observed.shots;e->scr_vehicle->turret.fireTime=100;}
double JavelinRotateDir(gentity_s*,const float* dir,const float*,float* out){std::memcpy(out,dir,3*sizeof(float));return observed.turn;}
void reset(int step,int time=0){observed=Observation{};level.frametime=step;level.time=time;climb.current.value=descent.current.value=0;
 climbCap.current.value=descentCap.current.value=10000;drag.current.value=.4f;}
gentity_s item(int trajectory=TR_LINEAR){gentity_s e;e.spawnflags=1;e.s.lerp.pos.trType=trajectory;e.r.currentOrigin[2]=100;
 e.s.lerp.pos.trBase[2]=100;e.s.lerp.pos.trTime=level.time;e.s.lerp.pos.trDelta[0]=.1f;e.r.ownerNum.value=77;return e;}
}
#include "native_snippets.hpp"
using namespace dependency;
struct Api {void(*item)(gentity_s*);void(*heli)(gentity_s*);void(*weapon)(gentity_s*);
 void(*jav)(gentity_s*,const float*,const float*,float*);int(*frame)();};
void itemContracts(Api candidate,Api old){
 for(int dt:{1,2,8,50}){
  reset(dt,1000);auto e=item();candidate.item(&e);near(e.r.currentOrigin[0],.1f*dt*.001f,.000001f);
  near(e.r.currentOrigin[2],100,.000001f);CHECK(observed.used==2);CHECK(observed.order=="TTLK");
  CHECK(observed.owners==std::vector<int>({77,77}));CHECK(observed.masks==std::vector<int>({0x123400,0x123400}));
  near(observed.ends[1][2],99); // Probe endpoint never becomes committed free motion.
  reset(dt,1000);auto ground=item();trace_t support;support.fraction=.005f;observed.traces={trace_t{},support};candidate.item(&ground);
  CHECK(ground.s.lerp.pos.trType==TR_STATIONARY);CHECK(ground.s.groundEntityNum==42);near(ground.r.currentOrigin[2],99.995f);
  CHECK(observed.order=="TTLKOL");CHECK((g_entities[42].flags&FL_GROUND_ENT)!=0);
  for(float normal:{0.f,-1.f}){reset(dt,1000);auto wall=item();support.normal[2]=normal;observed.traces={trace_t{},support};candidate.item(&wall);
   CHECK(wall.r.inuse);CHECK(wall.s.groundEntityNum==ENTITYNUM_NONE);near(wall.r.currentOrigin[2],100);CHECK(observed.order=="TTLK");}
  reset(dt,1000);auto far=item();support.normal[2]=1;support.fraction=.5f;observed.traces={trace_t{},support};candidate.item(&far);
  near(far.r.currentOrigin[2],100);CHECK(far.s.groundEntityNum==ENTITYNUM_NONE);
  reset(dt,1000);auto hit=item();hit.s.lerp.pos.trDelta[0]=1000;trace_t primary;primary.fraction=.5f;observed.traces={primary};candidate.item(&hit);
  CHECK(hit.s.lerp.pos.trType==TR_LINEAR_STOP);CHECK(hit.s.lerp.pos.trDuration==dt);
  near(hit.s.lerp.pos.trBase[0]+hit.s.lerp.pos.trDelta[0]*dt*.001f,hit.r.currentOrigin[0]);
  // Preserve native starts-solid resolution and the second wall-slide trace.
  reset(dt,1000);auto embedded=item();primary.fraction=0;primary.startsolid=true;
  primary.normal[0]=primary.normal[1]=0;primary.normal[2]=1;observed.traces={primary};candidate.item(&embedded);
  CHECK(observed.used==1);CHECK(observed.order=="TLKOL");CHECK(embedded.s.lerp.pos.trType==TR_STATIONARY);
  near(embedded.r.currentOrigin[2],100);CHECK(embedded.s.groundEntityNum==42);
  reset(dt,1000);auto slide=item();slide.s.lerp.pos.trDelta[0]=1000;
  primary.fraction=.005f;primary.startsolid=false;primary.normal[1]=1;primary.normal[2]=0;
  trace_t secondary;secondary.fraction=.5f;observed.traces={primary,secondary};candidate.item(&slide);
  CHECK(observed.used==2);CHECK(observed.order=="TTLK");CHECK(slide.s.lerp.pos.trDuration==dt);
  near(observed.starts[1][0],dt*.005f);near(observed.ends[1][1],1);
  near(slide.r.currentOrigin[0],dt*.5025f);near(slide.r.currentOrigin[1],.5f);
  for(int axis=0;axis<3;++axis)near(slide.s.lerp.pos.trBase[axis]+slide.s.lerp.pos.trDelta[axis]*dt*.001f,slide.r.currentOrigin[axis]);
  reset(dt,1000);auto unowned=item();unowned.r.ownerNum.value=-1;candidate.item(&unowned);
  CHECK(observed.owners==std::vector<int>({ENTITYNUM_NONE,ENTITYNUM_NONE}));
  // Moving support changes an ordinary item to native analytic gravity. A
  // missed one-unit support probe must not add one unit to its committed fall.
  reset(dt,1000);auto moving=item();moving.spawnflags=0;moving.s.groundEntityNum=42;
  g_entities[42].s.lerp.pos.trType=TR_LINEAR;candidate.item(&moving);
  CHECK(moving.s.lerp.pos.trType==TR_GRAVITY);CHECK(moving.s.lerp.pos.trTime==1000);
  near(moving.r.currentOrigin[2],100-400.f*dt*.001f*dt*.001f,.00001f);
  reset(dt,1000);auto supported=item(TR_STATIONARY);supported.spawnflags=0;supported.s.groundEntityNum=42;
  g_entities[42].s.lerp.pos.trType=TR_STATIONARY;candidate.item(&supported);
  CHECK(observed.order=="K");near(supported.r.currentOrigin[2],100);
  reset(dt,1000);auto freed=item();observed.freeOnThink=true;observed.traces={trace_t{},trace_t{.005f}};candidate.item(&freed);
  CHECK(!freed.r.inuse);CHECK(observed.order=="TTLK");
  reset(dt,1000);auto sky=item();observed.sky=true;observed.traces={trace_t{},trace_t{.005f}};candidate.item(&sky);
  CHECK(!sky.r.inuse);CHECK(observed.order=="TTLKF");
 }
 reset(1,1000);auto negative=item();old.item(&negative);near(negative.r.currentOrigin[2],99);CHECK(observed.used==1);
 // Both step sizes finish the same analytic elapsed-time horizon, with no missed support imported as motion.
 for(int dt:{1,2,8,50}){reset(dt);auto falling=item(TR_GRAVITY);falling.s.lerp.pos.trDelta[0]=0;
  for(int t=0;t<1000;t+=dt){level.time=t;candidate.item(&falling);}near(falling.r.currentOrigin[2],-300,.01f);}
 reset(50);auto reference=item(TR_GRAVITY);reference.s.lerp.pos.trDelta[0]=0;
 for(int t=0;t<1000;t+=50){level.time=t;old.item(&reference);}near(reference.r.currentOrigin[2],-300,.01f);
 reset(1);auto stationary=item(TR_STATIONARY);candidate.item(&stationary);CHECK(observed.order=="K");
 reset(1);auto tagged=item();tagged.tagInfo=&tagged;candidate.item(&tagged);CHECK(observed.order=="K");
}
void helicopterContracts(Api candidate,Api old){
 for(int dt:{1,2,8,50}){reset(dt);gentity_s e;for(int t=0;t<1000;++t){level.time=t;if(t>=e.nextthink)candidate.heli(&e);}
  CHECK(observed.heliCalls==unsigned(1000/dt));CHECK(std::fabs(observed.integratedSeconds-1)<.000001);}
 reset(1);gentity_s negative;for(int t=0;t<1000;++t){level.time=t;if(t>=negative.nextthink)old.heli(&negative);}
 CHECK(observed.heliCalls==20);CHECK(std::fabs(observed.integratedSeconds-.02)<.000001);
 reset(0);CHECK(candidate.frame()==1);reset(-4);CHECK(candidate.frame()==1);
 for(int dt:{1,2,50}){reset(dt);gentity_s turret;scr_vehicle_s vehicle;gclient_s gunner;turret.s.weapon=1;turret.scr_vehicle=&vehicle;
  g_entities[0].client=&gunner;gunner.buttons=BUTTON_ATTACK;for(int t=0;t<1000;t+=dt){level.time=t;candidate.weapon(&turret);}
  CHECK(observed.shots==10);gunner.buttons=0;for(int t=0;t<1000;t+=dt)candidate.weapon(&turret);CHECK(observed.shots==10);
  observed.gunner=ENTITYNUM_NONE;gunner.buttons=BUTTON_ATTACK;candidate.weapon(&turret);CHECK(observed.shots==10);}
}
float retained(Api api,int dt,float turn,float decel){reset(dt);observed.turn=turn;drag.current.value=decel;gentity_s e;
 float velocity[3]{100,0,0},target[3]{1,0,0};for(int t=0;t<1000;t+=dt)api.jav(&e,velocity,target,velocity);return Vec3Length(velocity);}
void javelinContracts(Api candidate,Api old){
 for(float turn:{0.f,15.f,30.f,45.f,90.f,180.f})for(float decel:{0.f,.4f,1.f}){
  const float stock=retained(old,50,turn,decel);near(retained(candidate,50,turn,decel),stock,.000001f);
  for(int dt:{1,2,8})near(retained(candidate,dt,turn,decel),stock,.012f);
 }
 CHECK(retained(old,1,90,.4f)<.001f);CHECK(retained(candidate,1,90,.4f)>.5f);
 // Changing controlled turn errors preserve the elapsed stock retention too.
 auto changing=[&](Api api,int dt){reset(dt);gentity_s e;float velocity[3]{100,0,0},target[3]{1,0,0};
  const float turns[]{0,15,45,90,180};for(int t=0;t<1000;t+=dt){observed.turn=turns[t/200];api.jav(&e,velocity,target,velocity);}
  return Vec3Length(velocity);};
 const float changedStock=changing(old,50);for(int dt:{1,2,8})near(changing(candidate,dt),changedStock,.012f);
 for(int stage:{MISSILESTAGE_ASCENT,MISSILESTAGE_DESCENT})for(int dt:{1,2,50}){
  reset(dt);observed.turn=0;climb.current.value=descent.current.value=2;gentity_s e;e.missile.stage=stage;
  float velocity[3]{50,0,0},target[3]{1,0,0};for(int t=0;t<1000;t+=dt)candidate.jav(&e,velocity,target,velocity);
  near(Vec3Length(velocity),52,.01f);
  climbCap.current.value=descentCap.current.value=51;candidate.jav(&e,velocity,target,velocity);near(Vec3Length(velocity),51,.001f);
 }
}
int main(){Api candidate{proposed::G_RunItem,proposed::Helicopter_Think,proposed::VEH_UpdateWeapon,proposed::JavelinRotateVelocity,proposed::G_VehicleFrameMsec};
 Api old{current::G_RunItem,current::Helicopter_Think,current::VEH_UpdateWeapon,current::JavelinRotateVelocity,current::G_VehicleFrameMsec};
 itemContracts(candidate,old);helicopterContracts(candidate,old);javelinContracts(candidate,old);
 std::printf("{\"passed\":true,\"assertions\":%u,\"native_function_source\":true,\"negative_controls\":3,\"loaded_map\":false,\"native_abi\":false,\"whole_world\":false}\n",checks);
}
