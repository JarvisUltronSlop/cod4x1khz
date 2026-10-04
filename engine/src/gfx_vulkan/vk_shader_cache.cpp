#include "vk_shader_cache.h"
#include "gfx/gfx_hash.h"
#include <cstring>
#include <fstream>

namespace gfx::vk {
LinkedShaders cachedLink(const std::filesystem::path& root,const Context& context,
    const Shader& vertex,const Shader& pixel,std::span<const MOJOSHADER_vertexAttribute> attributes) {
    if(root.empty()) return link(vertex,pixel,attributes);
    // Explicit versioning includes the bytecode emitter, linker, validation
    // environment, native backend lowering and device/driver identity. Padding
    // bytes and pointers never enter the key.
    constexpr char version[]="cod4x1khz-vk6:mojo-ad5dff84830c2863c841f4b1f4e3df78c705b383:spirv-2025.1:Vulkan1.3:legacy-color0:missing-input-pointcoord-offsets:implicit-legacy-inputs";
    std::vector<std::byte> key;
    auto append=[&](const void* data,std::size_t bytes){const auto* p=static_cast<const std::byte*>(data);key.insert(key.end(),p,p+bytes);};
    auto number=[&](std::uint32_t value){append(&value,4);};
    append(version,sizeof(version)); number(unsigned(vertex.bytecode.size())); append(vertex.bytecode.data(),vertex.bytecode.size()*4);
    number(unsigned(pixel.bytecode.size())); append(pixel.bytecode.data(),pixel.bytecode.size()*4); number(unsigned(attributes.size()));
    for(const auto& a:attributes) {number(unsigned(a.usage));number(unsigned(a.usageIndex));number(unsigned(a.vertexElementFormat));}
    const auto& device=context.selected.properties; number(device.vendorID);number(device.deviceID);number(device.driverVersion);append(device.pipelineCacheUUID,16);
    std::filesystem::create_directories(root); const auto path=root/(gfx::sha256(key)+".spirv");
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    constexpr std::uint32_t magic=0x324b4853;
    if(input&&input.tellg()>=12&&input.tellg()<=2*1024*1024) {
        const auto bytes=std::uint64_t(input.tellg());input.seekg(0);std::uint32_t header[3];input.read(reinterpret_cast<char*>(header),12);
        if(header[0]==magic&&header[1]>=5&&header[2]>=5&&12ull+4ull*(std::uint64_t(header[1])+header[2])==bytes) {
            LinkedShaders cached;cached.vertex.resize(header[1]);cached.pixel.resize(header[2]);
            input.read(reinterpret_cast<char*>(cached.vertex.data()),header[1]*4);input.read(reinterpret_cast<char*>(cached.pixel.data()),header[2]*4);
            if(input) {
                try {validateSpirv(cached.vertex,vertex.name.c_str());validateSpirv(cached.pixel,pixel.name.c_str());return cached;}
                catch(const std::exception&) { /* corrupt cache is rebuilt from the original stock bytecode */ }
            }
        }
    }
    auto result=link(vertex,pixel,attributes);auto partial=path;partial+=".partial";
    const std::uint32_t header[]={magic,unsigned(result.vertex.size()),unsigned(result.pixel.size())};
    {std::ofstream output(partial,std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char*>(header),12);output.write(reinterpret_cast<const char*>(result.vertex.data()),result.vertex.size()*4);output.write(reinterpret_cast<const char*>(result.pixel.data()),result.pixel.size()*4);if(!output) throw std::runtime_error("Translated shader cache write failed");}
    if(!MoveFileExW(partial.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Translated shader cache commit failed");
    return result;
}
}
