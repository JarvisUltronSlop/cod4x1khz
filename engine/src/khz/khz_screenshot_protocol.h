#pragma once
#include "khz_content.h"
#include <array>
#include <string_view>
namespace khz::screenshot {
inline constexpr unsigned Format=1,MaximumText=1023,ChunkBytes=512,MaximumBytes=1024*1024;
inline constexpr unsigned MaximumChunks=MaximumBytes/ChunkBytes;
inline constexpr unsigned MaximumWidth=1280,MaximumHeight=720,MaximumCaptureWidth=4096,MaximumCaptureHeight=2160;
inline constexpr unsigned MaximumCaptureBytes=MaximumCaptureWidth*MaximumCaptureHeight*3;
struct Request { std::uint64_t id=0,session=0;std::uint32_t generation=0;bool operator==(const Request&)const=default; };
struct Metadata { Request request{};unsigned width=0,height=0,bytes=0,chunks=0;content::Digest sha256{}; };
struct Chunk { Request request{};unsigned index=0,bytes=0;std::array<std::uint8_t,ChunkBytes> data{}; };
enum class Error:unsigned { Invalid=1,Busy=2,Stale=3,Capture=4,Encoding=5,Checksum=6,Storage=7,Timeout=8,Unavailable=9 };
struct Rejection { Request request{};Error error=Error::Invalid; };
bool ParseRequest(std::string_view text,Request* result);
bool ParseMetadata(std::string_view text,Metadata* result);
bool ParseChunk(std::string_view text,Chunk* result);
bool ParseRejection(std::string_view text,Rejection* result);
bool ParseDone(std::string_view text,Request* result);
bool FormatRequest(const Request& request,char* text,std::size_t capacity);
bool FormatMetadata(const Metadata& value,char* text,std::size_t capacity);
bool FormatChunk(const Chunk& value,char* text,std::size_t capacity);
bool FormatRejection(const Rejection& value,char* text,std::size_t capacity);
bool FormatDone(const Request& value,char* text,std::size_t capacity);
bool ValidRequest(const Request& value);
bool ValidMetadata(const Metadata& value);
bool SafeBasename(std::string_view value);
struct Connection {
    std::uint64_t session=0,lifecycle=0;
    std::uint32_t generation=0,slot=0;
    content::Digest fingerprint{};
    bool active=false,privateAddress=false,bot=false;
};
bool Admissible(const Connection& connection);
bool SameConnection(const Connection& expected,const Connection& current);
// Main admission only, no allocation/hash/codec/disk. Duplicate same bytes are
// harmless; conflicting duplicates and out-of-range/truncated chunks reject.
class Assembly {
public:
    enum class Accepted { Added,Duplicate,Complete,Rejected };
    bool Begin(const Metadata& metadata,std::uint8_t* storage,unsigned capacity);
    Accepted Put(const Chunk& chunk);
    void Reset();
    bool Complete()const{return metadata_.bytes&&received_==metadata_.chunks;}
    const Metadata& GetMetadata()const{return metadata_;}
private:
    Metadata metadata_{};std::uint8_t* storage_=nullptr;unsigned received_=0;
    std::array<std::uint64_t,MaximumChunks/64> seen_{};
};
}
