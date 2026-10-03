#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <equipment_policy.hpp>

namespace khz::unlock_audit {
constexpr unsigned StatsBytes=8192,StatCount=3498,SidecarBytes=20+StatCount*4,EncodedBytes=0x211c;
constexpr unsigned Capacity=524288,TableCapacity=393216;
constexpr unsigned XpIndex=2301;
constexpr unsigned offset(unsigned index){return index<2000?index+4:index*4-5996;}
inline std::uint32_t stat(const unsigned char* data,unsigned index){
    if(index<2000)return data[offset(index)];std::uint32_t value;std::memcpy(&value,data+offset(index),4);return value;
}
inline void putStat(unsigned char* data,unsigned index,std::uint32_t value){
    if(index<2000)data[offset(index)]=static_cast<unsigned char>(value);else std::memcpy(data+offset(index),&value,4);
}
inline bool ownershipIndex(unsigned index){return index<=200||(index>=250&&index<270)||(index>=3000&&index<3150);}
inline std::uint32_t crc32(const void* storage,std::size_t count){
    auto crc=~0u;const auto* data=static_cast<const unsigned char*>(storage);
    for(std::size_t i=0;i<count;++i){crc^=data[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u*(crc&1u));}return ~crc;
}
inline std::uint32_t fnv(const void* storage,std::size_t count){
    std::uint32_t hash=2166136261u;const auto* data=static_cast<const unsigned char*>(storage);
    for(std::size_t i=0;i<count;++i)hash=(hash^data[i])*16777619u;return hash;
}
enum class Failure{None,Missing,MaskIndex,MaskWidth,Candidate,Checksum,Sidecar,Capacity,Identity,Tables};
inline const char* failureName(Failure error){
    switch(error){case Failure::None:return "none";case Failure::Missing:return "missing_actual_transaction";
    case Failure::MaskIndex:return "stock_mask_outside_ownership";case Failure::MaskWidth:return "byte_stat_mask_width";
    case Failure::Candidate:return "candidate_has_unauthorized_change";case Failure::Checksum:return "candidate_payload_checksum";
    case Failure::Sidecar:return "sidecar_disagrees_with_transaction";case Failure::Capacity:return "audit_fixed_capacity";
    case Failure::Identity:return "audit_profile_or_backup_identity";case Failure::Tables:return "actual_stock_tables_unavailable";}return "unknown";
}
inline Failure validate(const unsigned char* before,const unsigned char* candidate,
    const std::uint32_t* masks,unsigned minimumXp,const unsigned char* sidecar){
    if(!before||!candidate||!masks||!minimumXp||!sidecar)return Failure::Missing;
    for(unsigned i=0;i<StatCount;++i){
        if(masks[i]&&!ownershipIndex(i))return Failure::MaskIndex;
        if(i<2000&&masks[i]>255)return Failure::MaskWidth;
        const auto old=stat(before,i),now=stat(candidate,i);
        const auto expected=i==XpIndex?(old<minimumXp?minimumXp:old):(old|masks[i]);
        if(now!=expected)return Failure::Candidate;
    }
    // All bytes beyond the named stat representation also remain unchanged;
    // this closes the gap left by merely comparing named combat stat hashes.
    constexpr unsigned payloadEnd=offset(StatCount-1)+4;
    if(std::memcmp(before+payloadEnd,candidate+payloadEnd,StatsBytes-payloadEnd))return Failure::Candidate;
    unsigned header;std::memcpy(&header,candidate,4);
    if(header!=crc32(candidate+4,StatsBytes-4))return Failure::Checksum;
    std::uint32_t fields[5];std::memcpy(fields,sidecar,sizeof(fields));
    if(fields[0]!=0x3155484bu||fields[1]!=1||fields[2]!=StatCount||fields[3]!=minimumXp||
        std::memcmp(sidecar+20,masks,StatCount*4))return Failure::Sidecar;
    auto hash=2166136261u;for(unsigned i=0;i<SidecarBytes;++i)hash=(hash^(i>=16&&i<20?0:sidecar[i]))*16777619u;
    if(hash!=fields[4])return Failure::Sidecar;
    return Failure::None;
}
class Bytes {
public:
    Bytes(unsigned char* storage,unsigned capacity):data_(storage),capacity_(capacity){}
    bool ok()const{return good_;}unsigned size()const{return used_;}
    void raw(const void* value,unsigned size){
        if(!good_||!value||size>capacity_-used_){good_=false;return;}
        std::memcpy(data_+used_,value,size);used_+=size;
    }
    void u16(unsigned value){if(value>65535){good_=false;return;}unsigned char b[2]={static_cast<unsigned char>(value),static_cast<unsigned char>(value>>8)};raw(b,2);}
    void u32(std::uint32_t value){unsigned char b[4];for(unsigned i=0;i<4;++i)b[i]=static_cast<unsigned char>(value>>(i*8));raw(b,4);}
    void u64(std::uint64_t value){unsigned char b[8];for(unsigned i=0;i<8;++i)b[i]=static_cast<unsigned char>(value>>(i*8));raw(b,8);}
    bool text(const char* value,unsigned maximum){
        if(!value){good_=false;return false;}unsigned length=0;while(length<=maximum&&value[length])++length;
        if(length>maximum){good_=false;return false;}u16(length);raw(value,length);return good_;
    }
private:unsigned char* data_;unsigned capacity_,used_=0;bool good_=true;
};
struct Evidence {
    const unsigned char *before,*candidate,*sidecar,*encoded,*tables;
    const std::uint32_t* masks;
    unsigned minimumXp,tablesBytes;
    const char *home,*profile,*backup;
    bool backupCompleted,encodedValidated,stockTablesValidated;
};
// Output bytes are unpublished until the real completed commit supplies all
// evidence. The caller stores serial/size only after this function succeeds.
inline Failure serialize(Bytes& bytes,std::uint64_t serial,const Evidence& value){
    if(!serial||!value.backupCompleted||!value.encodedValidated||!value.stockTablesValidated||!value.encoded)return Failure::Missing;
    const auto error=validate(value.before,value.candidate,value.masks,value.minimumXp,value.sidecar);if(error!=Failure::None)return error;
    if(!value.tables||value.tablesBytes<4||value.tablesBytes>TableCapacity)return Failure::Tables;
    bytes.raw("KHZUA075",8);bytes.u32(1);bytes.u64(serial);bytes.u32(StatCount);bytes.u32(StatsBytes);bytes.u32(value.minimumXp);
    bytes.u32(crc32(value.before+4,StatsBytes-4));bytes.u32(crc32(value.candidate+4,StatsBytes-4));
    bytes.u32(SidecarBytes);bytes.u32(EncodedBytes);bytes.u32(value.tablesBytes);
    if(!bytes.text(value.home,1023)||!bytes.text(value.profile,95)||!bytes.text(value.backup,95))return Failure::Identity;
    bytes.raw(value.before,StatsBytes);bytes.raw(value.candidate,StatsBytes);
    for(unsigned i=0;i<StatCount;++i)bytes.u32(value.masks[i]);
    bytes.raw(value.sidecar,SidecarBytes);bytes.raw(value.encoded,EncodedBytes);bytes.raw(value.tables,value.tablesBytes);
    return bytes.ok()?Failure::None:Failure::Capacity;
}
} // namespace khz::unlock_audit
