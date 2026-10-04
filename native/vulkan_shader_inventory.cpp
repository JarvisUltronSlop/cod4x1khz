#include "gfx_vulkan/vk_shader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

std::string jsonString(const std::string& text) {
    std::string out="\"";const char* hex="0123456789abcdef";
    for(unsigned char c:text) { if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c); }return out+'"';
}
int main(int argc,char** argv) {
    if(argc!=3) {std::cerr<<"Usage: khz_vulkan_shader_inventory <private shader directory> <report.json>\n";return 2;}
    try {
        std::vector<std::filesystem::path> files;
        for(const auto& e:std::filesystem::directory_iterator(argv[1])) if(e.is_regular_file()&&e.path().extension()==".dxbc") files.push_back(e.path());
        std::sort(files.begin(),files.end()); if(files.empty()) throw std::runtime_error("No captured stock shaders");
        std::ofstream report(argv[2]); if(!report) throw std::runtime_error("Cannot create inventory report");unsigned passed=0,failed=0;
        report<<"{\"schema\":1,\"scope\":\"Captured stock bytecode parser and Vulkan emitter; stage linkage and stock game visuals require separate validation\",\"stock_rendering_verified\":false,\"shaders\":[";
        for(std::size_t n=0;n<files.size();++n) {
            if(n) report<<','; report<<"{\"file\":"<<jsonString(files[n].filename().string());
            try {
                const auto bytes=std::filesystem::file_size(files[n]);if(bytes<8||bytes%4||bytes>65536*4) throw std::runtime_error("Invalid captured bytecode size");
                std::vector<std::uint32_t> words(bytes/4);std::ifstream input(files[n],std::ios::binary);input.read(reinterpret_cast<char*>(words.data()),bytes);if(!input) throw std::runtime_error("Captured bytecode read failed");
                gfx::vk::Shader shader(words,files[n].filename().string());
                report<<",\"passed\":true,\"stage\":"<<jsonString(shader.parsed->shader_type==MOJOSHADER_TYPE_VERTEX?"vertex":"pixel")<<",\"shader_model\":\""<<shader.parsed->major_ver<<'.'<<shader.parsed->minor_ver<<"\",\"samplers\":"<<shader.parsed->sampler_count<<",\"uniform_bytes\":"<<shader.uniformBytes<<",\"instructions\":"<<shader.parsed->instruction_count<<'}';++passed;
            } catch(const std::exception& e) {report<<",\"passed\":false,\"error\":"<<jsonString(e.what())<<'}';++failed;}
        }
        report<<"],\"total\":"<<files.size()<<",\"translated\":"<<passed<<",\"failed\":"<<failed<<",\"passed\":"<<(failed?"false":"true")<<"}\n";
        std::cout<<"Stock bytecode emitter: "<<passed<<'/'<<files.size()<<" translated, "<<failed<<" explicit errors\n";return failed?1:0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
