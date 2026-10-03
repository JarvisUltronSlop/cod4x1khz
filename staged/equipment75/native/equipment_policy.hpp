#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>

// Source proposal only. The native reader and its standalone tests share these
// bounds and the same named-field projection; no asset registration is needed.
namespace khz::equipment {
constexpr unsigned WeaponCapacity = 128, ModelCapacity = 16, PerkCapacity = 20;
constexpr unsigned OwnedCapacity = 16, TextCapacity = 192, JsonCapacity = 32768;
enum class GenerationNamespace { HumanAdmission, BotAdmission, ClientOwner };
struct Admission {
    std::uint64_t session,expectedSession,generation,expectedGeneration;
    GenerationNamespace generationNamespace;
    unsigned receivedStatsMask;
    bool activeHuman,provenIdentity,matchingNativeStorage,playing,playingTeam;
    int health,lastSpawnTime;
};
inline const char* admissionFailure(const Admission& value){
    if(!value.session||value.session!=value.expectedSession)return "expected_native_session_mismatch";
    if(value.generationNamespace!=GenerationNamespace::HumanAdmission||!value.generation||value.generation!=value.expectedGeneration)
        return "expected_human_admission_generation_mismatch";
    if(!value.activeHuman||!value.provenIdentity||!value.matchingNativeStorage)return "actual_active_human_identity_mismatch";
    if(value.receivedStatsMask!=127)return "received_stats_packet_mask_incomplete";
    if(!value.playing||!value.playingTeam||value.health<=0||value.lastSpawnTime<=0)return "requires_live_spawned_human";
    return nullptr;
}
struct Weapon {
    unsigned index, alternate, model;
    int inventoryType, offhandClass, ammoIndex, clipIndex;
    char name[TextCapacity], gunModel[TextCapacity], worldModel[TextCapacity];
    bool gunModelPresent, worldModelPresent;
};
struct Player {
    int clientNum, commandTime, pmType, weaponState, offhand, offhandSecondary;
    unsigned current, shotCount, perks, ownedCount,catalogueCount;
    std::uint32_t owned[4];
    int ammo[WeaponCapacity], clips[WeaponCapacity];
    std::uint8_t models[WeaponCapacity];
    Weapon weapons[OwnedCapacity];
};
enum class Failure {
    None, Catalogue, CurrentIndex, OffhandIndex, OwnershipIndex, OwnedCapacity,
    ModelIndex, Definition, AlternateIndex, AmmoIndex, ClipIndex, Text, Perks
};
inline const char* failureName(Failure failure) {
    switch (failure) {
    case Failure::None:return "none"; case Failure::Catalogue:return "native_weapon_catalogue_bounds";
    case Failure::CurrentIndex:return "current_weapon_index"; case Failure::OffhandIndex:return "offhand_weapon_index";
    case Failure::OwnershipIndex:return "owned_weapon_index"; case Failure::OwnedCapacity:return "owned_weapon_capacity";
    case Failure::ModelIndex:return "weapon_model_index_16"; case Failure::Definition:return "native_weapon_definition_missing";
    case Failure::AlternateIndex:return "alternate_weapon_index"; case Failure::AmmoIndex:return "native_ammo_index";
    case Failure::ClipIndex:return "native_clip_index"; case Failure::Text:return "asset_name_missing_or_unterminated";
    case Failure::Perks:return "unsupported_native_perk_bits";
    } return "unknown";
}
template<std::size_t N> bool copyText(char (&destination)[N], const char* source) {
    if (!source) return false;
    for (std::size_t i=0;i<N;++i) {
        destination[i]=source[i]; if (!source[i]) return true;
    }
    destination[N-1]=0; return false; // Never accept a truncated identifier.
}
template<class T, std::size_t N> const T* modelAt(T* const (&models)[N], unsigned model) {
    static_assert(N == ModelCapacity, "stock native model arrays have sixteen entries");
    return model<N ? models[model] : nullptr;
}
inline bool validIndex(unsigned index, unsigned count) { return index<WeaponCapacity && index<count; }

// Catalogue.count/describe are read-only adapters. describe is called only
// after both weapon and model indices are checked, including active unowned
// current/offhand entries. A failure never publishes a partial Player record.
template<class NativePlayer, class Catalogue>
Failure read(const NativePlayer& source, const Catalogue& catalogue, Player& output) {
    const auto count=catalogue.count();
    if (!count || count>WeaponCapacity) return Failure::Catalogue;
    if (!validIndex(source.weapon,count)) return Failure::CurrentIndex;
    if (source.offHandIndex<0 || !validIndex(static_cast<unsigned>(source.offHandIndex),count)) return Failure::OffhandIndex;
    if (static_cast<unsigned>(source.perks)&~((1u<<PerkCapacity)-1u)) return Failure::Perks;
    output.clientNum=source.clientNum; output.commandTime=source.commandTime;
    output.pmType=static_cast<int>(source.pm_type); output.weaponState=static_cast<int>(source.weaponstate);
    output.offhand=source.offHandIndex; output.offhandSecondary=static_cast<int>(source.offhandSecondary);
    output.current=source.weapon; output.shotCount=source.weaponShotCount;
    output.perks=static_cast<unsigned>(source.perks); output.ownedCount=0;output.catalogueCount=count;
    std::memcpy(output.owned,source.weapons,sizeof(output.owned));
    std::memcpy(output.ammo,source.ammo,sizeof(output.ammo));
    std::memcpy(output.clips,source.ammoclip,sizeof(output.clips));
    std::memcpy(output.models,source.weaponmodels,sizeof(output.models));
    for (unsigned index=0;index<WeaponCapacity;++index) {
        const bool owned=(output.owned[index/32]&(1u<<(index%32)))!=0;
        const bool active=index==output.current || index==static_cast<unsigned>(output.offhand);
        if (!owned&&!active) continue;
        if (!validIndex(index,count) || (owned&&!index)) return Failure::OwnershipIndex;
        if (output.models[index]>=ModelCapacity) return Failure::ModelIndex;
        // Index zero is the explicit native no-weapon sentinel; not equipment.
        if (!index) continue;
        if (output.ownedCount==OwnedCapacity) return Failure::OwnedCapacity;
        auto& weapon=output.weapons[output.ownedCount];
        std::memset(&weapon,0,sizeof(weapon)); weapon.index=index; weapon.model=output.models[index];
        if (!catalogue.describe(index,weapon.model,weapon)) return Failure::Definition;
        if (!validIndex(weapon.alternate,count)) return Failure::AlternateIndex;
        if (weapon.ammoIndex<0||weapon.ammoIndex>=static_cast<int>(WeaponCapacity)) return Failure::AmmoIndex;
        if (weapon.clipIndex<0||weapon.clipIndex>=static_cast<int>(WeaponCapacity)) return Failure::ClipIndex;
        if (!weapon.name[0]) return Failure::Text;
        ++output.ownedCount;
    }
    return Failure::None;
}
class Json {
public:
    Json(char* storage,std::size_t capacity):data_(storage),capacity_(capacity) { if(capacity_)data_[0]=0; }
    bool ok()const{return valid_;} std::size_t size()const{return used_;}
    void raw(const char* value) { if(!value){valid_=false;return;} const auto n=std::strlen(value); append(value,n); }
    void number(const char* format,...) {
        if(!valid_||used_>=capacity_)return;
        va_list args;va_start(args,format);const int n=std::vsnprintf(data_+used_,capacity_-used_,format,args);va_end(args);
        if(n<0||static_cast<std::size_t>(n)>=capacity_-used_){valid_=false;return;}used_+=static_cast<std::size_t>(n);
    }
    void text(const char* value) {
        raw("\""); if(!value){valid_=false;return;}
        for(unsigned i=0;value[i];++i){
            if(i>=TextCapacity){valid_=false;return;}
            const auto ch=static_cast<unsigned char>(value[i]);
            if(ch=='\"'||ch=='\\'){const char pair[2]={'\\',static_cast<char>(ch)};append(pair,2);}
            else if(ch<32||ch>126)number("\\u%04x",ch);else append(value+i,1);
        }raw("\"");
    }
    template<class T,std::size_t N>void array(const T (&values)[N]){
        raw("[");for(std::size_t i=0;i<N;++i){if(i)raw(",");number("%lld",static_cast<long long>(values[i]));}raw("]");
    }
private:
    void append(const char* value,std::size_t count){
        if(!valid_||!capacity_||count>=capacity_-used_){valid_=false;return;}
        std::memcpy(data_+used_,value,count);used_+=count;data_[used_]=0;
    }
    char* data_;std::size_t capacity_,used_=0;bool valid_=true;
};
inline void playerJSON(Json& json,const Player& value,const char* source){
    json.raw("{\"source\":");json.text(source);
    json.number(",\"client_num\":%d,\"command_time\":%d,\"pm_type\":%d,\"weapon\":%u,\"weapon_state\":%d,\"weapon_shot_count\":%u,\"offhand\":%d,\"offhand_secondary\":%d,\"perks\":%u,\"weapon_catalogue_count\":%u,\"owned\":",value.clientNum,value.commandTime,value.pmType,value.current,value.weaponState,value.shotCount,value.offhand,value.offhandSecondary,value.perks,value.catalogueCount);
    json.array(value.owned);json.raw(",\"ammo\":");json.array(value.ammo);json.raw(",\"clips\":");json.array(value.clips);
    json.raw(",\"models\":");json.array(value.models);json.raw(",\"weapons\":[");
    for(unsigned i=0;i<value.ownedCount;++i){const auto& weapon=value.weapons[i];if(i)json.raw(",");
        const bool owned=(value.owned[weapon.index/32]&(1u<<(weapon.index%32)))!=0;
        json.number("{\"index\":%u,\"owned\":%s,\"model\":%u,\"inventory_type\":%d,\"offhand_class\":%d,\"alternate\":%u,\"ammo_index\":%d,\"clip_index\":%d,\"name\":",weapon.index,owned?"true":"false",weapon.model,weapon.inventoryType,weapon.offhandClass,weapon.alternate,weapon.ammoIndex,weapon.clipIndex);
        json.text(weapon.name);json.raw(",\"gun_model\":");if(weapon.gunModelPresent)json.text(weapon.gunModel);else json.raw("null");
        json.raw(",\"world_model\":");if(weapon.worldModelPresent)json.text(weapon.worldModel);else json.raw("null");json.raw("}");
    }json.raw("]}");
}
} // namespace khz::equipment
