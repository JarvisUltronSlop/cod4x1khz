#include "gfx_vulkan/vk_shader.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <algorithm>

std::string quoted(const std::string& s){std::string o="\"";const char* h="0123456789abcdef";for(unsigned char c:s){if(c=='"'||c=='\\'){o+='\\';o+=char(c);}else if(c<32){o+="\\u00";o+=h[c>>4];o+=h[c&15];}else o+=char(c);}return o+'"';}
int main(int argc,char** argv) {
    if(argc!=4){std::cerr<<"Usage: khz_vulkan_shader_link <private shader dir> <pairs.txt> <report.json>\n";return 2;}
    try {
        std::map<std::string,std::unique_ptr<gfx::vk::Shader>> shaders;
        auto shader=[&](const std::string& digest)->gfx::vk::Shader& {
            if(auto found=shaders.find(digest);found!=shaders.end()) return *found->second;
            if(digest.size()!=64||digest.find_first_not_of("0123456789abcdef")!=std::string::npos) throw std::runtime_error("Invalid stock shader digest");
            const auto path=std::filesystem::path(argv[1])/(digest+".dxbc");const auto bytes=std::filesystem::file_size(path);if(bytes%4||bytes>65536*4) throw std::runtime_error("Invalid captured stock bytecode length");
            std::vector<std::uint32_t> words(bytes/4);std::ifstream file(path,std::ios::binary);file.read(reinterpret_cast<char*>(words.data()),bytes);if(!file) throw std::runtime_error("Stock shader read failed");
            auto& result=shaders[digest];result=std::make_unique<gfx::vk::Shader>(words,digest);return *result;
        };
        std::ifstream input(argv[2]);std::ofstream report(argv[3]);if(!input||!report) throw std::runtime_error("Cannot open material pair manifest/report");
        unsigned total=0,failed=0;report<<"{\"schema\":1,\"scope\":\"Actual stock shader stage pairs and vertex declarations linked and validated as Vulkan 1.3 SPIR-V; GPU material/visual acceptance is separate\",\"stock_rendering_verified\":false,\"pairs\":[";
        std::string row;while(std::getline(input,row)) {
            if(row.empty()) continue;if(total) report<<',';++total;std::istringstream fields(row);std::string vs,ps;unsigned count=0;fields>>vs>>ps>>count;
            report<<"{\"ordinal\":"<<total<<",\"vertex\":"<<quoted(vs)<<",\"pixel\":"<<quoted(ps);
            try {
                if(count>32) throw std::runtime_error("Stock declaration exceeds input bound");std::vector<MOJOSHADER_vertexAttribute> attributes;
                for(unsigned n=0;n<count;++n){int usage,index,format;if(!(fields>>usage>>index>>format)||usage<0||usage>=MOJOSHADER_USAGE_TOTAL||index<0||index>15||format<0||format>11) throw std::runtime_error("Malformed material declaration manifest");attributes.push_back({MOJOSHADER_usage(usage),MOJOSHADER_vertexElementFormat(format),index});}
                auto& vertex=shader(vs);auto& pixel=shader(ps);
                for(int n=0;n<vertex.parsed->attribute_count;++n){const auto& a=vertex.parsed->attributes[n];if(a.usage==MOJOSHADER_USAGE_UNKNOWN) continue;vertex.inputRegister(a.usage,a.index);if(std::none_of(attributes.begin(),attributes.end(),[&](const auto& d){return d.usage==a.usage&&d.usageIndex==a.index;})) throw std::runtime_error("Stock declaration lacks used shader input");}
                const auto result=gfx::vk::link(vertex,pixel,attributes);report<<",\"passed\":true,\"vertex_words\":"<<result.vertex.size()<<",\"pixel_words\":"<<result.pixel.size()<<'}';
            }catch(const std::exception& e){report<<",\"passed\":false,\"error\":"<<quoted(e.what())<<'}';++failed;}
        }
        report<<"],\"total\":"<<total<<",\"failed\":"<<failed<<",\"passed\":"<<(total&&!failed?"true":"false")<<"}\n";std::cout<<"Stock stage linkage: "<<total-failed<<'/'<<total<<", "<<failed<<" explicit errors\n";return failed||!total?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
