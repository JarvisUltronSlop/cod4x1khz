#include "gfx_vulkan/vk_shader.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <array>
int main(int argc,char**argv) {
    if(argc!=2)return 2;
    try {
        const auto bytes=std::filesystem::file_size(argv[1]);
        if(!bytes||bytes%4||bytes>65536*4)return 2;
        std::vector<std::uint32_t> words(bytes/4);std::ifstream input(argv[1],std::ios::binary);
        input.read(reinterpret_cast<char*>(words.data()),bytes);if(!input)return 2;
        gfx::vk::Shader shader(words,argv[1]);
        std::array<float,256*4> floats{};std::array<int,16*4> integers{};std::array<int,16> booleans{};
        for(unsigned n=0;n<floats.size();++n)floats[n]=float(n);
        std::vector<float> packed(shader.uniformBytes/4);
        shader.packUniforms(packed.data(),floats,integers,booleans);
        std::cout<<"bytes="<<shader.uniformBytes<<" uniforms="<<shader.parsed->uniform_count<<" literals="<<shader.parsed->constant_count<<'\n';
        unsigned offset=0;
        for(int n=0;n<shader.parsed->uniform_count;++n){const auto& u=shader.parsed->uniforms[n];
            std::cout<<"uniform type="<<u.type<<" index="<<u.index<<" count="<<u.array_count<<" constant="<<u.constant<<" offset="<<offset;
            for(unsigned j=0;j<unsigned((std::max)(1,u.array_count))*4;++j)std::cout<<' '<<packed[offset/4+j];
            std::cout<<'\n';offset+=unsigned((std::max)(1,u.array_count))*16;
        }
        for(int n=0;n<shader.parsed->constant_count;++n){const auto& k=shader.parsed->constants[n];std::cout<<"literal type="<<k.type<<" index="<<k.index;for(auto v:k.value.f)std::cout<<' '<<v;std::cout<<'\n';}
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
