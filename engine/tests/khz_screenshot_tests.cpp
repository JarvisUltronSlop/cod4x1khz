#include <khz/khz_screenshot_protocol.h>
#include <khz/khz_screenshot_image.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace wire=khz::screenshot;
unsigned cases=0,failures=0;
void check(bool value,const char* name){++cases;if(!value){++failures;std::printf("FAIL: %s\n",name);}}
wire::Metadata metadata(unsigned bytes){wire::Metadata value;value.request={42,123,17};value.width=640;value.height=480;
    value.bytes=bytes;value.chunks=(bytes+wire::ChunkBytes-1)/wire::ChunkBytes;value.sha256[0]=1;return value;}
void codecs(){
    char text[wire::MaximumText+1];wire::Request request{UINT64_MAX,UINT64_MAX,UINT32_MAX},parsed;
    check(wire::FormatRequest(request,text,sizeof(text))&&wire::ParseRequest(text,&parsed)&&parsed==request,"maximum exact request numbers round trip");
    check(wire::FormatDone(request,text,sizeof(text))&&wire::ParseDone(text,&parsed)&&parsed==request,"done round trip");
    check(!wire::FormatRequest({0,1,1},text,sizeof(text)),"zero ID rejected");
    check(!wire::FormatRequest(request,text,10),"small formatting buffer rejected");
    constexpr const char* bad[]={"khz_ss_request 2 1 2 3","khz_ss_request 1 0 2 3","khz_ss_request 1 1 0 3","khz_ss_request 1 1 2 0",
        "khz_ss_request 1 01 2 3","khz_ss_request 1 -1 2 3","khz_ss_request 1 +1 2 3","khz_ss_request 1 1 2 4294967296",
        "khz_ss_request 1 18446744073709551616 2 3","khz_ss_request 1 1 2 3 extra","khz_ss_request 1 1 2 3 ",
        "khz_ss_request  1 1 2 3","khz_ss_request\t1 1 2 3","khz_ss_request 1 1 2 3\n","khz_ss_request 1 1 2 3;quit"};
    for(const auto* value:bad)check(!wire::ParseRequest(value,&parsed),"malformed request refused");
    wire::Metadata full=metadata(wire::MaximumBytes),decoded;
    check(wire::FormatMetadata(full,text,sizeof(text))&&wire::ParseMetadata(text,&decoded)&&decoded.bytes==full.bytes&&decoded.sha256==full.sha256,"maximum metadata round trip");
    auto invalid=full;invalid.chunks--;check(!wire::ValidMetadata(invalid),"wrong chunk count rejected");
    invalid=full;invalid.bytes++;check(!wire::ValidMetadata(invalid),"payload exceeds fixed bound rejected");
    invalid=full;invalid.width=wire::MaximumWidth+1;check(!wire::ValidMetadata(invalid),"oversize image dimensions rejected");
    invalid=full;invalid.sha256={};check(!wire::ValidMetadata(invalid),"empty digest rejected");
    wire::Rejection rejection{{9,10,11},wire::Error::Checksum},returned;
    check(wire::FormatRejection(rejection,text,sizeof(text))&&wire::ParseRejection(text,&returned)&&returned.request==rejection.request&&returned.error==rejection.error,"error round trip");
    check(!wire::ParseRejection("khz_ss_error 1 1 2 3 0",&returned)&&!wire::ParseRejection("khz_ss_error 1 1 2 3 10",&returned),"unknown errors rejected");
    for(unsigned size:std::array<unsigned,8>{1,2,3,4,255,510,511,512}){
        wire::Chunk chunk,copy;chunk.request=request;chunk.index=wire::MaximumChunks-1;chunk.bytes=size;
        for(unsigned i=0;i<size;++i)chunk.data[i]=std::uint8_t(i*31+size);
        check(wire::FormatChunk(chunk,text,sizeof(text))&&std::strlen(text)<=wire::MaximumText&&wire::ParseChunk(text,&copy)
            &&copy.request==chunk.request&&copy.index==chunk.index&&copy.bytes==size&&!std::memcmp(chunk.data.data(),copy.data.data(),size),"bounded canonical chunk round trip");
    }
    constexpr const char* malformed[]={"khz_ss_chunk 1 1 2 3 0 A===","khz_ss_chunk 1 1 2 3 0 ====","khz_ss_chunk 1 1 2 3 0 AA=A",
        "khz_ss_chunk 1 1 2 3 0 AB==","khz_ss_chunk 1 1 2 3 0 AAB=","khz_ss_chunk 1 1 2 3 0 AA==AAAA",
        "khz_ss_chunk 1 1 2 3 2048 AA==","khz_ss_chunk 1 1 2 3 0 _A==","khz_ss_chunk 1 1 2 3 0 AAA"};
    wire::Chunk copy;for(const auto* value:malformed)check(!wire::ParseChunk(value,&copy),"malformed base64 chunk refused");
}
void assembly(){
    std::vector<std::uint8_t> storage(wire::MaximumBytes);wire::Assembly receiver;auto descriptor=metadata(1025);
    check(!receiver.Begin(descriptor,storage.data(),1024),"assembly enforces storage capacity");
    check(receiver.Begin(descriptor,storage.data(),unsigned(storage.size())),"assembly starts bounded payload");
    wire::Chunk last;last.request=descriptor.request;last.index=2;last.bytes=1;last.data[0]=7;
    check(receiver.Put(last)==wire::Assembly::Accepted::Added&&!receiver.Complete(),"last chunk accepted out of order");
    check(receiver.Put(last)==wire::Assembly::Accepted::Duplicate,"identical duplicate harmless");
    last.data[0]=8;check(receiver.Put(last)==wire::Assembly::Accepted::Rejected,"conflicting duplicate refused");last.data[0]=7;
    auto wrong=last;wrong.request.session++;check(receiver.Put(wrong)==wire::Assembly::Accepted::Rejected,"cross-session chunk refused");
    wrong=last;wrong.request.generation++;check(receiver.Put(wrong)==wire::Assembly::Accepted::Rejected,"cross-map chunk refused");
    wrong=last;wrong.request.id++;check(receiver.Put(wrong)==wire::Assembly::Accepted::Rejected,"cross-request chunk refused");
    wrong=last;wrong.bytes=2;check(receiver.Put(wrong)==wire::Assembly::Accepted::Rejected,"wrong final length refused");
    wrong=last;wrong.index=3;check(receiver.Put(wrong)==wire::Assembly::Accepted::Rejected,"outside metadata chunk range refused");
    wire::Chunk chunk;chunk.request=descriptor.request;chunk.bytes=wire::ChunkBytes;chunk.data.fill(1);
    chunk.index=1;check(receiver.Put(chunk)==wire::Assembly::Accepted::Added,"middle chunk accepted");
    chunk.index=0;check(receiver.Put(chunk)==wire::Assembly::Accepted::Complete&&receiver.Complete()&&storage[1024]==7,"out-of-order payload completes exactly");
    receiver.Reset();check(!receiver.Complete()&&receiver.Put(chunk)==wire::Assembly::Accepted::Rejected,"reset invalidates previous frame");
    descriptor=metadata(wire::MaximumBytes);check(receiver.Begin(descriptor,storage.data(),unsigned(storage.size())),"maximum bounded assembly begins");
    bool exact=true;for(unsigned i=0;i<wire::MaximumChunks;++i){chunk.index=wire::MaximumChunks-1-i;chunk.request=descriptor.request;
        const auto value=receiver.Put(chunk);exact&=value==(i+1==wire::MaximumChunks?wire::Assembly::Accepted::Complete:wire::Assembly::Accepted::Added);}
    check(exact&&receiver.Complete(),"all2048 chunks reverse order stay within bound");
}
void lifecycle(){
    wire::Connection current;current.session=3;current.generation=4;current.lifecycle=5;current.slot=1;current.fingerprint[0]=1;
    current.active=current.privateAddress=true;check(wire::Admissible(current)&&wire::SameConnection(current,current),"accepted private human lifecycle");
    for(unsigned field=0;field<9;++field){auto other=current;
        switch(field){case 0:other.session++;break;case 1:other.generation++;break;case 2:other.lifecycle++;break;case 3:other.slot++;break;
            case 4:other.fingerprint[0]++;break;case 5:other.active=false;break;case 6:other.privateAddress=false;break;case 7:other.bot=true;break;case 8:other.fingerprint={};break;}
        check(!wire::SameConnection(current,other),"changed admission/session/map/address/activity refused");}
    check(wire::SafeBasename("player_1-test")&&wire::SafeBasename(std::string(48,'a')),"safe bounded output basename");
    for(const auto* bad:std::array<const char*,8>{"","../a","a/b","a\\b","a.b","a:b","a b","a\n"})check(!wire::SafeBasename(bad),"unsafe output name refused");
    check(!wire::SafeBasename(std::string(49,'a')),"long output name refused");
}
void images(){
    std::vector<std::uint8_t> pixels(641*479*3);for(std::size_t i=0;i<pixels.size();++i)pixels[i]=std::uint8_t(i*7);
    std::vector<std::uint8_t> jpeg;unsigned width=0,height=0;
    const bool encoded=wire::EncodeJpeg(pixels.data(),641,479,&jpeg,&width,&height);
    check(encoded&&width==641&&height==479&&jpeg.size()<=wire::MaximumBytes,"actual WIC JPEG odd packed stride encode");
    check(encoded&&wire::ValidateJpeg(jpeg.data(),unsigned(jpeg.size()),width,height),"actual WIC JPEG full decode/dimensions");
    check(encoded&&!wire::ValidateJpeg(jpeg.data(),unsigned(jpeg.size()),width+1,height),"JPEG descriptor dimension mismatch refused");
    check(encoded&&!wire::ValidateJpeg(jpeg.data(),unsigned(jpeg.size()-1),width,height),"truncated JPEG refused");
    auto corrupted=jpeg;if(corrupted.size()>2)corrupted[0]=0;
    check(encoded&&!wire::ValidateJpeg(corrupted.data(),unsigned(corrupted.size()),width,height),"invalid JPEG marker refused");
    std::array<std::uint8_t,4> emptyJpeg{255,216,255,217};check(!wire::ValidateJpeg(emptyJpeg.data(),4,640,480),"header-only fake JPEG refused");
    pixels.assign(1920*1080*3,64);check(wire::EncodeJpeg(pixels.data(),1920,1080,&jpeg,&width,&height)&&width==1280&&height==720
        &&wire::ValidateJpeg(jpeg.data(),unsigned(jpeg.size()),width,height),"1080p capture resizes to bounded720p JPEG");
    check(!wire::EncodeJpeg(nullptr,640,480,&jpeg,&width,&height)&&!wire::EncodeJpeg(pixels.data(),wire::MaximumCaptureWidth+1,1,&jpeg,&width,&height),"invalid capture bounds refused");
    khz::content::Digest digest{};check(khz::content::Sha256(jpeg.data(),jpeg.size(),&digest),"actual screenshot SHA256 computed off tick");
    auto changed=jpeg;if(!changed.empty())changed.back()^=1;khz::content::Digest modified{};
    check(khz::content::Sha256(changed.data(),changed.size(),&modified)&&digest!=modified,"modified screenshot hash differs");
}
}
int main(){codecs();assembly();lifecycle();images();std::printf("Screenshot codec/lifecycle/WIC fixtures: %u cases, %u failures\n",cases,failures);return failures?1:0;}
