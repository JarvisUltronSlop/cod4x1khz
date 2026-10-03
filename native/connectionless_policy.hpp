#pragma once
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace khz::connectionless {
enum class Owner { Frontend, Connection, Lifecycle };
struct Token { unsigned char data[1024]{}; std::size_t size=0; };
inline bool whitespace(unsigned char value){return value<=32&&value!=20&&value!=21&&value!=22;}
// MSG_ReadStringLine keeps at most1023 bytes, replaces '%' with '.' and cleans
// Latin1 smart apostrophes. Cmd's first token skips repeated block comments,
// accepts unclosed quotes, and ends unquoted tokens at either comment opener.
// Decode only that bounded native line, without shared MSG/Cmd scratch or heap.
inline Token firstToken(const unsigned char* packet,std::size_t size){
    Token token;
    if(!packet||size<5||packet[0]!=255||packet[1]!=255||packet[2]!=255||packet[3]!=255)return token;
    std::size_t length=0;
    while(length<1023&&length<size-4&&packet[4+length]&&packet[4+length]!='\n')++length;
    const auto character=[&](std::size_t index)->unsigned char{
        if(index>=length)return 0;
        const auto value=packet[4+index];return value=='%'?'.':value==146?'\'':value;
    };
    std::size_t cursor=0;
    for(;;){
        while(cursor<length&&whitespace(character(cursor)))++cursor;
        if(cursor>=length||(character(cursor)=='/'&&character(cursor+1)=='/'))return token;
        if(character(cursor)!='/'||character(cursor+1)!='*')break;
        cursor+=2;
        while(cursor<length&&(character(cursor)!='*'||character(cursor+1)!='/'))++cursor;
        if(cursor>=length)return token;
        cursor+=2;
    }
    if(character(cursor)=='"'){
        ++cursor;
        while(cursor<length&&character(cursor)!='"'){
            if(character(cursor)=='\\'&&character(cursor+1)=='"')++cursor;
            token.data[token.size++]=character(cursor++);
        }
    }else{
        do{token.data[token.size++]=character(cursor++);}
        while(cursor<length&&!whitespace(character(cursor))&&
            (character(cursor)!='/'||(character(cursor+1)!='/'&&character(cursor+1)!='*')));
    }
    return token;
}
inline Owner classify(const unsigned char* packet, std::size_t size) {
    const auto token=firstToken(packet,size);
    auto matches = [&](std::string_view name) {
        if (token.size != name.size()) return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            auto value = token.data[i];
            if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
            auto wanted = static_cast<unsigned char>(name[i]);
            if (wanted >= 'A' && wanted <= 'Z') wanted += 'a' - 'A';
            if (value != wanted) return false;
        }
        return true;
    };
    for (const auto name : {"loadingnewmap", "requeststats", "disconnect", "error"})
        if (matches(name)) return Owner::Lifecycle;
    for (const auto name : {"challengeResponse", "connectResponse", "statResponse",
        "fastrestart", "print", "v"})
        if (matches(name)) return Owner::Connection;
    // Native dispatch uses a case-sensitive nine-byte prefix here.
    constexpr std::string_view needKey="needcdkey";
    if(token.size>=needKey.size()){
        bool prefix=true;for(std::size_t i=0;i<needKey.size();++i)if(token.data[i]!=static_cast<unsigned char>(needKey[i]))prefix=false;
        if(prefix)return Owner::Connection;
    }
    return Owner::Frontend;
}
} // namespace khz::connectionless
