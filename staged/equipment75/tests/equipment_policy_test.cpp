#include <equipment_policy.hpp>
#include <unlock_audit_policy.hpp>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {
int checks=0,failed=0;
void check(bool valid,const char* name){++checks;if(!valid){++failed;std::printf("FAIL %s\n",name);}}
struct PS{
    int clientNum=0,commandTime=17,pm_type=0,weaponstate=0,offHandIndex=2,offhandSecondary=1;
    unsigned weapon=1,weaponShotCount=3;int perks=1;
    std::uint32_t weapons[4]{};int ammo[128]{},ammoclip[128]{};std::uint8_t weaponmodels[128]{};
};
struct Catalog{
    unsigned number=4;mutable unsigned lookups=0;int ammo=1,clip=1;unsigned alternate=0;
    unsigned count()const{return number;}
    bool describe(unsigned index,unsigned model,khz::equipment::Weapon& value)const{
        ++lookups;value.alternate=alternate;value.ammoIndex=ammo;value.clipIndex=clip;
        return index<number&&model<16&&khz::equipment::copyText(value.name,"fixture_weapon");
    }
};
unsigned char before[8192],candidate[8192],sidecar[khz::unlock_audit::SidecarBytes];
std::uint32_t masks[khz::unlock_audit::StatCount];
void u32(unsigned char* p,unsigned value){std::memcpy(p,&value,4);}
void prepare(){
    using namespace khz::unlock_audit;
    for(unsigned i=0;i<sizeof(before);++i)before[i]=static_cast<unsigned char>(i*13);
    std::memset(masks,0,sizeof(masks));masks[25]=1;masks[200]=1;masks[3000]=0x4001;
    putStat(before,XpIndex,100);std::memcpy(candidate,before,sizeof(before));
    for(unsigned i=0;i<StatCount;++i)putStat(candidate,i,stat(before,i)|masks[i]);putStat(candidate,XpIndex,200);
    u32(candidate,crc32(candidate+4,8188));
    std::memset(sidecar,0,sizeof(sidecar));u32(sidecar,0x3155484b);u32(sidecar+4,1);u32(sidecar+8,StatCount);u32(sidecar+12,200);
    std::memcpy(sidecar+20,masks,sizeof(masks));u32(sidecar+16,fnv(sidecar,sizeof(sidecar)));
}
}
int main(){
    using namespace khz::equipment;Player player{};Catalog catalog;PS ps;ps.weapons[0]=(1u<<1)|(1u<<2);
    check(read(ps,catalog,player)==Failure::None&&player.ownedCount==2,"actual named projection and offhand");
    ps.weapon=128;catalog.lookups=0;check(read(ps,catalog,player)==Failure::CurrentIndex&&!catalog.lookups,"current bound before native getter");ps.weapon=1;
    ps.offHandIndex=-1;check(read(ps,catalog,player)==Failure::OffhandIndex,"negative offhand refused");ps.offHandIndex=2;
    ps.weapons[3]=1u<<31;catalog.lookups=0;check(read(ps,catalog,player)==Failure::OwnershipIndex,"owned bit127 outside actual catalogue refused");ps.weapons[3]=0;
    ps.weaponmodels[1]=15;check(read(ps,catalog,player)==Failure::None,"model15 is stock array last valid entry");
    ps.weaponmodels[1]=16;catalog.lookups=0;check(read(ps,catalog,player)==Failure::ModelIndex&&!catalog.lookups,"model16 rejected before any model getter");
    ps.weaponmodels[1]=255;check(read(ps,catalog,player)==Failure::ModelIndex,"model255 byte is not array authorization");ps.weaponmodels[1]=0;
    catalog.ammo=-1;check(read(ps,catalog,player)==Failure::AmmoIndex,"negative ammo index");catalog.ammo=128;check(read(ps,catalog,player)==Failure::AmmoIndex,"ammo128");catalog.ammo=1;
    catalog.clip=128;check(read(ps,catalog,player)==Failure::ClipIndex,"clip128");catalog.clip=1;
    catalog.alternate=127;check(read(ps,catalog,player)==Failure::AlternateIndex,"alternate outside live catalogue");catalog.alternate=0;
    ps.perks=1<<20;check(read(ps,catalog,player)==Failure::Perks,"unknown perk bit refused");ps.perks=1;
    catalog.number=129;check(read(ps,catalog,player)==Failure::Catalogue,"catalogue129 refused");catalog.number=128;
    ps.weapons[0]=0xfffffffe;check(read(ps,catalog,player)==Failure::OwnedCapacity,"explicit fixed inventory capacity fails closed");
    struct Model{int marker;};Model first{1},last{15};Model* models[16]{};models[0]=&first;models[15]=&last;
    check(modelAt(models,0)==&first&&modelAt(models,15)==&last,"checkerboard model pointer boundaries");
    check(modelAt(models,16)==nullptr&&modelAt(models,255)==nullptr,"no out-of-array model reads");
    models[15]=nullptr;check(modelAt(models,15)==nullptr,"actual missing optional model stays null");
    Admission admission{1,1,12,12,GenerationNamespace::HumanAdmission,127,true,true,true,true,true,100,1};
    check(!admissionFailure(admission),"actual complete human admission");
    admission.receivedStatsMask=126;check(admissionFailure(admission),"one missing received stats block fails");admission.receivedStatsMask=255;check(admissionFailure(admission),"extra received stats bits fail");admission.receivedStatsMask=127;
    admission.generationNamespace=GenerationNamespace::BotAdmission;check(admissionFailure(admission),"same numeric generation in bot namespace fails");admission.generationNamespace=GenerationNamespace::ClientOwner;check(admissionFailure(admission),"owner generation cannot attest server human");admission.generationNamespace=GenerationNamespace::HumanAdmission;
    admission.expectedGeneration=13;check(admissionFailure(admission),"slot reuse generation fails");admission.expectedGeneration=12;
    admission.expectedSession=2;check(admissionFailure(admission),"old session fails");admission.expectedSession=1;
    admission.lastSpawnTime=0;check(admissionFailure(admission),"unspawned human cannot prove loadout");admission.lastSpawnTime=1;
    admission.health=0;check(admissionFailure(admission),"dead diagnostic cannot prove live equipment");
    char tiny[4];Json overflow(tiny,sizeof(tiny));overflow.raw("abcdef");check(!overflow.ok(),"bounded JSON cannot report truncated success");
    char escaped[64];Json json(escaped,sizeof(escaped));json.text("quote\"\\\n");check(json.ok()&&!std::strcmp(escaped,"\"quote\\\"\\\\\\u000a\""),"asset strings are escaped");
    using AuditFailure=khz::unlock_audit::Failure;
    prepare();check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::None,"real candidate all-stat invariant");
    candidate[205]^=2;check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::Candidate,"unchanged class stat mutation rejected");prepare();
    masks[2300]=1;check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::MaskIndex,"combat counter cannot become ownership mask");prepare();
    masks[25]=256;check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::MaskWidth,"byte masks cannot overflow");prepare();
    candidate[8191]^=1;check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::Candidate,"trailing unnamed payload byte preserved");prepare();
    candidate[0]^=1;check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::Checksum,"candidate native CRC header validated");prepare();
    sidecar[30]^=1;check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::Sidecar,"sidecar masks must exactly match transaction");prepare();
    sidecar[16]^=1;check(khz::unlock_audit::validate(before,candidate,masks,200,sidecar)==AuditFailure::Sidecar,"sidecar independent FNV integrity");
    check(khz::unlock_audit::crc32("123456789",9)==0xcbf43926u,"stock CRC32 known vector");
    std::printf("equipment/audit policy: %d cases, %d failures\n",checks,failed);return failed?1:0;
}
