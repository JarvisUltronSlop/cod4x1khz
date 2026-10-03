#include "khz_screenshot_protocol.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>

namespace khz::screenshot {
namespace {
struct Tokens {
    std::array<std::string_view,11> values{};unsigned count=0;
    bool Read(std::string_view text){if(text.empty()||text.size()>MaximumText)return false;for(unsigned char c:text)if(c<32||c>126)return false;
        while(!text.empty()){if(count==values.size())return false;const auto end=text.find(' ');auto word=text.substr(0,end);if(word.empty())return false;values[count++]=word;
            if(end==text.npos)break;text.remove_prefix(end+1);if(text.empty())return false;}return true;}
};
bool number(std::string_view text,std::uint64_t* value){if(text.empty()||text.size()>20||(text.size()>1&&text[0]=='0'))return false;const auto p=std::from_chars(text.data(),text.data()+text.size(),*value);return p.ec==std::errc{}&&p.ptr==text.data()+text.size();}
bool integer(std::string_view text,unsigned* result){std::uint64_t value=0;if(!number(text,&value)||value>(std::numeric_limits<unsigned>::max)())return false;*result=unsigned(value);return true;}
bool request(const Tokens& tokens,const char* name,unsigned count,Request* value){if(tokens.count!=count||tokens.values[0]!=name||tokens.values[1]!="1")return false;
    Request parsed;unsigned generation=0;if(!number(tokens.values[2],&parsed.id)||!number(tokens.values[3],&parsed.session)||!integer(tokens.values[4],&generation))return false;parsed.generation=generation;if(!ValidRequest(parsed))return false;*value=parsed;return true;}
char digit(unsigned value){return "0123456789abcdef"[value&15];}
bool digest(std::string_view text,content::Digest* result){if(text.size()!=64)return false;for(unsigned i=0;i<32;++i){unsigned value=0;for(unsigned half=0;half<2;++half){const auto c=text[2*i+half];if(c>='0'&&c<='9')value=value*16+c-'0';else if(c>='a'&&c<='f')value=value*16+c-'a'+10;else return false;}(*result)[i]=std::uint8_t(value);}return true;}
int base64value(char c){if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;}
bool decode(std::string_view text,Chunk* result){if(text.empty()||text.size()%4||text.size()>((ChunkBytes+2)/3)*4)return false;unsigned count=0;
    for(unsigned at=0;at<text.size();at+=4){const int a=base64value(text[at]),b=base64value(text[at+1]);if(a<0||b<0||count>=ChunkBytes)return false;
        const bool last=at+4==text.size(),one=text[at+2]=='=',two=text[at+3]=='=';const int c=one?0:base64value(text[at+2]),d=two?0:base64value(text[at+3]);
        if(c<0||d<0||(one&&!two)||((one||two)&&!last)||(one&&(b&15))||(!one&&two&&(c&3)))return false;
        result->data[count++]=std::uint8_t((a<<2)|(b>>4));if(!one){if(count>=ChunkBytes)return false;result->data[count++]=std::uint8_t((b<<4)|(c>>2));}
        if(!two){if(count>=ChunkBytes)return false;result->data[count++]=std::uint8_t((c<<6)|d);}
    }result->bytes=count;return count>0;
}
struct Writer {
    char* output;std::size_t capacity,used=0;bool valid=true;
    bool Text(std::string_view text){if(!valid||!output||capacity<=used+text.size()||used+text.size()>MaximumText){valid=false;return false;}std::memcpy(output+used,text.data(),text.size());used+=text.size();output[used]=0;return true;}
    void Number(std::uint64_t value){char text[24];const auto p=std::to_chars(text,text+sizeof(text),value);Text(std::string_view(text,p.ptr-text));}
    void Field(std::uint64_t value){Text(" ");Number(value);}
    void Header(const char* name,const Request& value){Text(name);Field(1);Field(value.id);Field(value.session);Field(value.generation);}
};
}
bool ValidRequest(const Request& value){return value.id&&value.session&&value.generation;}
bool ValidMetadata(const Metadata& value){return ValidRequest(value.request)&&value.width&&value.width<=MaximumWidth&&value.height&&value.height<=MaximumHeight
    &&value.bytes&&value.bytes<=MaximumBytes&&value.chunks==(value.bytes+ChunkBytes-1)/ChunkBytes
    &&std::any_of(value.sha256.begin(),value.sha256.end(),[](auto c){return c!=0;});}
bool ParseRequest(std::string_view text,Request* result){Tokens tokens;return result&&tokens.Read(text)&&request(tokens,"khz_ss_request",5,result);}
bool ParseDone(std::string_view text,Request* result){Tokens tokens;return result&&tokens.Read(text)&&request(tokens,"khz_ss_done",5,result);}
bool ParseMetadata(std::string_view text,Metadata* result){Tokens tokens;Metadata value;if(!result||!tokens.Read(text)||!request(tokens,"khz_ss_meta",10,&value.request)
    ||!integer(tokens.values[5],&value.width)||!integer(tokens.values[6],&value.height)||!integer(tokens.values[7],&value.bytes)||!integer(tokens.values[8],&value.chunks)||!digest(tokens.values[9],&value.sha256)||!ValidMetadata(value))return false;*result=value;return true;}
bool ParseChunk(std::string_view text,Chunk* result){Tokens tokens;Chunk value;if(!result||!tokens.Read(text)||!request(tokens,"khz_ss_chunk",7,&value.request)
    ||!integer(tokens.values[5],&value.index)||value.index>=MaximumChunks||!decode(tokens.values[6],&value))return false;*result=value;return true;}
bool ParseRejection(std::string_view text,Rejection* result){Tokens tokens;Rejection value;unsigned error=0;if(!result||!tokens.Read(text)||!request(tokens,"khz_ss_error",6,&value.request)
    ||!integer(tokens.values[5],&error)||error<1||error>unsigned(Error::Unavailable))return false;value.error=Error(error);*result=value;return true;}
bool FormatRequest(const Request& value,char* text,std::size_t capacity){if(!ValidRequest(value))return false;Writer writer{text,capacity};writer.Header("khz_ss_request",value);return writer.valid;}
bool FormatDone(const Request& value,char* text,std::size_t capacity){if(!ValidRequest(value))return false;Writer writer{text,capacity};writer.Header("khz_ss_done",value);return writer.valid;}
bool FormatMetadata(const Metadata& value,char* text,std::size_t capacity){if(!ValidMetadata(value))return false;Writer writer{text,capacity};writer.Header("khz_ss_meta",value.request);writer.Field(value.width);writer.Field(value.height);writer.Field(value.bytes);writer.Field(value.chunks);writer.Text(" ");char encoded[64];for(unsigned i=0;i<32;++i){encoded[2*i]=digit(value.sha256[i]>>4);encoded[2*i+1]=digit(value.sha256[i]);}writer.Text(std::string_view(encoded,64));return writer.valid;}
bool FormatRejection(const Rejection& value,char* text,std::size_t capacity){if(!ValidRequest(value.request)||unsigned(value.error)<1||unsigned(value.error)>unsigned(Error::Unavailable))return false;Writer writer{text,capacity};writer.Header("khz_ss_error",value.request);writer.Field(unsigned(value.error));return writer.valid;}
bool FormatChunk(const Chunk& value,char* text,std::size_t capacity){if(!ValidRequest(value.request)||value.index>=MaximumChunks||!value.bytes||value.bytes>ChunkBytes)return false;Writer writer{text,capacity};writer.Header("khz_ss_chunk",value.request);writer.Field(value.index);writer.Text(" ");constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";char encoded[((ChunkBytes+2)/3)*4];unsigned count=0;
    for(unsigned at=0;at<value.bytes;at+=3){const unsigned left=value.bytes-at,a=value.data[at],b=left>1?value.data[at+1]:0,c=left>2?value.data[at+2]:0;
        encoded[count++]=alphabet[a>>2];encoded[count++]=alphabet[((a&3)<<4)|(b>>4)];encoded[count++]=left>1?alphabet[((b&15)<<2)|(c>>6)]:'=';encoded[count++]=left>2?alphabet[c&63]:'=';}
    writer.Text(std::string_view(encoded,count));return writer.valid;
}
bool Assembly::Begin(const Metadata& value,std::uint8_t* storage,unsigned capacity){if(!ValidMetadata(value)||!storage||capacity<value.bytes)return false;Reset();metadata_=value;storage_=storage;return true;}
Assembly::Accepted Assembly::Put(const Chunk& value){if(!metadata_.bytes||value.request!=metadata_.request||value.index>=metadata_.chunks)return Accepted::Rejected;
    const unsigned offset=value.index*ChunkBytes,expected=(std::min)(ChunkBytes,metadata_.bytes-offset);if(value.bytes!=expected)return Accepted::Rejected;
    auto& word=seen_[value.index/64];const auto bit=1ull<<(value.index%64);
    if(word&bit)return std::memcmp(storage_+offset,value.data.data(),expected)?Accepted::Rejected:Accepted::Duplicate;
    std::memcpy(storage_+offset,value.data.data(),expected);word|=bit;++received_;return Complete()?Accepted::Complete:Accepted::Added;
}
void Assembly::Reset(){metadata_={};storage_=nullptr;received_=0;seen_.fill(0);}
bool SafeBasename(std::string_view value){if(value.empty()||value.size()>48)return false;for(char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;return true;}
bool Admissible(const Connection& value){bool identity=false;for(auto byte:value.fingerprint)identity|=byte!=0;return value.active&&value.privateAddress&&!value.bot&&identity&&value.session&&value.generation&&value.lifecycle&&value.slot<64;}
bool SameConnection(const Connection& expected,const Connection& current){return Admissible(expected)&&Admissible(current)&&expected.session==current.session&&expected.lifecycle==current.lifecycle&&expected.generation==current.generation&&expected.slot==current.slot&&expected.fingerprint==current.fingerprint;}
}
