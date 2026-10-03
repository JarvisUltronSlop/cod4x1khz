#include <connectionless_policy.hpp>
#include <cstdio>
#include <string>

int main() {
    using khz::connectionless::Owner;
    unsigned cases = 0, failures = 0;
    auto check = [&](const std::string& data, Owner expected) {
        ++cases;
        if (khz::connectionless::classify(reinterpret_cast<const unsigned char*>(data.data()), data.size()) != expected) {
            ++failures; std::printf("FAIL classification: %s\n", data.c_str());
        }
    };
    const std::string prefix(4, '\xff');
    for (const char* name : {"loadingnewmap", "requeststats", "disconnect", "error"}) {
        check(prefix + name + "\n", Owner::Lifecycle);
        check(prefix + "\t\"" + name + "\" args\n", Owner::Lifecycle);
    }
    for (const char* name : {"challengeResponse", "connectResponse", "statResponse", "fastrestart", "print", "needcdkey", "v"})
        check(prefix + name + " more", Owner::Connection);
    for (const char* name : {"infoResponse", "statusResponse", "getserversResponse", "updateResponse", "khz_ui quit", "khz_stats", "requeststatsInvalid"})
        check(prefix + name + "\n", Owner::Frontend);
    check(prefix + "REQUESTSTATS\n", Owner::Lifecycle);
    check(prefix + "\"requeststats", Owner::Lifecycle); // native accepts an unclosed first quote
    check("requeststats\n", Owner::Frontend);
    check(prefix, Owner::Frontend);
    check(prefix + std::string("v\0binary", 8), Owner::Connection);
    check(prefix + "statResponse", Owner::Connection);
    for(const char* name:{"requeststats","loadingnewmap","disconnect","error"}){
        check(prefix+"/* prior */\t/*again*/"+name+"\n",Owner::Lifecycle);
        check(prefix+name+"/* trailing */",Owner::Lifecycle);
        check(prefix+name+"// trailing",Owner::Lifecycle);
        check(prefix+"\v\f\r "+name+"\fargs",Owner::Lifecycle);
        check(prefix+"\""+name+"\"suffix",Owner::Lifecycle); // suffix starts the second native token
        check(prefix+"\""+name+"\nignored",Owner::Lifecycle);
    }
    check(prefix+"//requeststats",Owner::Frontend);
    check(prefix+"/* requeststats",Owner::Frontend);
    check(prefix+"/* comment\n*/requeststats",Owner::Frontend); // native comments cannot cross MSG's first line
    check(prefix+"\nrequeststats",Owner::Frontend);
    check(prefix+std::string("\0requeststats",13),Owner::Frontend);
    check(prefix+"needcdkey-extra",Owner::Connection);
    check(prefix+"NEEDCDKEY-extra",Owner::Frontend); // native prefix check is case-sensitive
    check(prefix+"needcdke",Owner::Frontend);
    for(unsigned char special:{20,21,22}){
        check(prefix+std::string(1,char(special))+"requeststats",Owner::Frontend);
        check(prefix+"requeststats"+std::string(1,char(special)),Owner::Frontend);
    }
    check(prefix+std::string(1023,' ')+"requeststats",Owner::Frontend);
    check(prefix+std::string(1011,' ')+"requeststats",Owner::Lifecycle);
    check(prefix+std::string(1012,' ')+"requeststats",Owner::Frontend); // truncated native command
    auto tokenCheck=[&](const std::string& input,const std::string& expected){
        ++cases;const auto token=khz::connectionless::firstToken(reinterpret_cast<const unsigned char*>(input.data()),input.size());
        if(std::string(reinterpret_cast<const char*>(token.data),token.size)!=expected)++failures;
    };
    tokenCheck(prefix+"\"requeststats\\\"\"","requeststats\"");
    tokenCheck(prefix+"requeststats%","requeststats.");
    tokenCheck(prefix+std::string("needcdkey\x92",10),"needcdkey'");
    tokenCheck(prefix+"/*a*/\r/*b*/\"statResponse\"args","statResponse");
    check(prefix+"\"requeststats\\\"\"",Owner::Frontend); // escaped quote is part of the first token
    std::printf("cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
