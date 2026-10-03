#pragma once
#include <initializer_list>
#include <string_view>

namespace khz::service_control {
// Reviewed native MP phases: disconnected0/cinematic1/logo2 precede the
// connecting3 state. A shell video must not suspend its browser workers.
inline bool DisconnectedShell(int state){return state>=0&&state<3;}
// Native MP free0/zombie1/connected2/loading3/active4. Reliable control
// replies are needed while loading, before gameplay can become active.
inline bool ReliableServerPhase(int state){return state>=1&&state<=4;}
inline bool ContentReliable(std::string_view text){
    if(text.size()>1023)return false;
    for(const auto prefix:{std::string_view("khz_content_meta"),std::string_view("khz_content_error")})
        if(text.size()>=prefix.size()&&text.substr(0,prefix.size())==prefix&&(text.size()==prefix.size()||text[prefix.size()]==' '))return true;
    return false;
}
}
