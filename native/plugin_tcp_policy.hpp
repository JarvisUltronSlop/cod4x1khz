#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace khz::plugin_tcp::policy {
struct Endpoint {std::array<char,256> host{};std::uint16_t port=0;};
inline bool Parse(std::string_view text,Endpoint* output){
    if(!output||text.empty()||text.size()>255)return false;
    std::string_view host=text,port;
    if(text.front()=='['){
        const auto end=text.find(']');if(end==std::string_view::npos||end<2)return false;
        host=text.substr(1,end-1);
        if(end+1<text.size()){if(text[end+1]!=':')return false;port=text.substr(end+2);if(port.empty())return false;}
    }else{
        const auto first=text.find(':');
        if(first!=std::string_view::npos&&first==text.rfind(':')){host=text.substr(0,first);port=text.substr(first+1);if(port.empty())return false;}
    }
    if(host.empty()||host.size()>255)return false;
    for(unsigned char value:host)
        if(!((value>='a'&&value<='z')||(value>='A'&&value<='Z')||(value>='0'&&value<='9')
            ||value=='.'||value=='-'||value=='_'||value==':'||value=='%'))return false;
    unsigned number=28960; // Pinned NET_StringToAdr default PORT_SERVER.
    if(!port.empty()){
        if(port.size()>5)return false;number=0;
        for(unsigned char digit:port){if(digit<'0'||digit>'9')return false;number=number*10+digit-'0';}
        if(!number||number>65535)return false;
    }
    Endpoint candidate;std::memcpy(candidate.host.data(),host.data(),host.size());candidate.port=static_cast<std::uint16_t>(number);
    *output=candidate;return true;
}
}
