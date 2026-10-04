#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"Usage: khz_shader_disassemble <private reference directory>\n";return 2;}
    try{
        unsigned count=0;
        for(const auto& file:std::filesystem::directory_iterator(argv[1])){
            if(!file.is_regular_file()||file.path().extension()!=".bin"||file.path().filename().string().find("reference-shader-")!=0)continue;
            const auto size=file.file_size();if(size<8||size%4||size>65536*4)throw std::runtime_error("Invalid shader bytecode length");
            std::vector<char> bytes(size);std::ifstream input(file.path(),std::ios::binary);input.read(bytes.data(),bytes.size());if(!input)throw std::runtime_error("Cannot read private shader");
            ID3DBlob* assembly=nullptr;const auto result=D3DDisassemble(bytes.data(),bytes.size(),0,nullptr,&assembly);
            if(FAILED(result)||!assembly)throw std::runtime_error("Stock shader disassembly failed");
            auto destination=file.path();destination.replace_extension(".asm");std::ofstream output(destination,std::ios::binary);
            output.write(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());assembly->Release();if(!output)throw std::runtime_error("Cannot write private shader assembly");++count;
        }
        if(!count)throw std::runtime_error("No captured shader bytecode");
        std::cout<<"Disassembled "<<count<<" captured shaders\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
