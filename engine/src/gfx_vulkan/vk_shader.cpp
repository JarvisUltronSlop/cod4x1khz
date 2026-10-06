#include "vk_shader.h"
#include <spirv-tools/libspirv.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <spirv/unified1/spirv.hpp>
#include <unordered_set>

namespace gfx::vk {
namespace {
void integerPixelPosition(std::vector<std::uint32_t>& words) {
    // Stock PS3 VPOS has integer pixel centers. The Vulkan builtin is offset
    // by half a pixel, independent of the viewport's matching raster offset.
    // Lower only the stock loads; later depth-bias lowering still reads raw Z.
    unsigned variable=0,type=0,scalar=0;
    for(std::size_t at=5;at<words.size();at+=words[at]>>16)
        if((words[at]&65535)==spv::OpDecorate && (words[at]>>16)==4 &&
            words[at+2]==spv::DecorationBuiltIn && words[at+3]==spv::BuiltInFragCoord)variable=words[at+1];
    if(!variable)return;
    for(std::size_t at=5;at<words.size();at+=words[at]>>16)
        if((words[at]&65535)==spv::OpLoad && words[at+3]==variable)type=words[at+1];
    for(std::size_t at=5;at<words.size();at+=words[at]>>16)
        if((words[at]&65535)==spv::OpTypeVector && words[at+1]==type && words[at+3]==4)scalar=words[at+2];
    if(!type||!scalar)throw std::runtime_error("Stock pixel position lacks a float4 load");
    auto next=words[3];const auto half=next++,zero=next++,offset=next++;
    std::vector<std::uint32_t> out(words.begin(),words.begin()+5);out.reserve(words.size()+32);
    bool constants=false;
    for(std::size_t at=5;at<words.size();) {
        const auto count=words[at]>>16,op=words[at]&65535;
        if(op==spv::OpFunction&&!constants) {
            const std::uint32_t values[]={4u<<16|spv::OpConstant,scalar,half,0x3f000000,
                4u<<16|spv::OpConstant,scalar,zero,0,
                7u<<16|spv::OpConstantComposite,type,offset,half,half,zero,zero};
            out.insert(out.end(),std::begin(values),std::end(values));constants=true;
        }
        const auto first=out.size();out.insert(out.end(),words.begin()+at,words.begin()+at+count);
        if(op==spv::OpLoad&&words[at+3]==variable) {
            const auto raw=next++;out[first+2]=raw;
            const std::uint32_t subtract[]={5u<<16|spv::OpFSub,type,words[at+2],raw,offset};
            out.insert(out.end(),std::begin(subtract),std::end(subtract));
        }
        at+=count;
    }
    out[3]=next;words.swap(out);
}
// DX9 VS1.1 includes DCL records for its vertex declaration. Reject an
// undeclared input before the pinned emitter dereferences its missing record.
// This also gives the actual semantic/register mapping rather than assuming
// the earlier D3D8 fixed mapping applies to every DX9 legacy shader.
unsigned legacyInput(std::span<const std::uint32_t> words, MOJOSHADER_usage usage, unsigned index) {
    constexpr unsigned arguments[]={0,2,3,3,4,3,2,2,3,3,3,3,3,3,2,2,2,3,4,2,3,3,3,3,3};
    unsigned declared=0, found=UINT32_MAX;
    for(std::size_t at=1;at<words.size();) {
        const auto op=words[at]&65535;
        if(op==65535) return found;
        unsigned count=0;
        if(op==65534) count=(words[at]>>16)&32767;
        else if(op<std::size(arguments)) count=arguments[op];
        else if(op==31||op==78||op==79) count=2;
        else if(op==81) count=5;
        else throw std::runtime_error("Unsupported VS1.1 instruction");
        if(count>=words.size()-at) throw std::runtime_error("Truncated VS1.1 instruction");
        if(op==31) {
            const auto semantic=words[at+1],reg=words[at+2]&2047;
            if(reg>=16) throw std::runtime_error("Invalid VS1.1 input register");
            declared|=1u<<reg;
            if((semantic&15)==unsigned(usage)&&((semantic>>16)&15)==index) found=reg;
        } else if(op!=81&&op!=65534) for(unsigned n=2;n<=count;++n) {
            const auto token=words[at+n],type=((token>>28)&7)|((token>>8)&24),reg=token&2047;
            if(type==1&&(reg>=16||!(declared&(1u<<reg)))) throw std::runtime_error("VS1.1 input lacks a DX9 vertex declaration");
        }
        at+=count+1;
    }
    throw std::runtime_error("VS1.1 shader lacks an end token");
}
}
Shader::Shader(std::span<const std::uint32_t> source, std::string debugName) : bytecode(source.begin(), source.end()), name(std::move(debugName)) {
    if (source.empty() || source.size() > 65536) throw std::runtime_error("Invalid stock shader bytecode length");
    if(source[0]==0xfffe0101) legacyInput(source,MOJOSHADER_USAGE_UNKNOWN,0);
    const auto* result = MOJOSHADER_parse(MOJOSHADER_PROFILE_SPIRV, "main",
        reinterpret_cast<const unsigned char*>(source.data()), unsigned(source.size_bytes()), nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr);
    parsed.reset(result);
    if (!result || result->error_count || result->output_len <= 0) {
        std::string message = "Stock shader translation failed: " + name;
        if (result) for (int i = 0; i < result->error_count; ++i) message += "\n" + std::string(result->errors[i].error);
        throw std::runtime_error(message);
    }
    uniformCopies.reserve(result->uniform_count);
    for(int i=0;i<result->uniform_count;++i){
        const auto& u=result->uniforms[i];const auto count=unsigned((std::max)(1,u.array_count));
        if(u.index<0)throw std::runtime_error("Invalid shader constant register");
        if(u.type!=MOJOSHADER_UNIFORM_FLOAT&&u.type!=MOJOSHADER_UNIFORM_INT&&u.type!=MOJOSHADER_UNIFORM_BOOL)throw std::runtime_error("Unsupported shader uniform type");
        uniformCopies.push_back({u.type,unsigned(u.index),count,unsigned(uniformBytes)});
        // DEF/DEFI/DEFB overrides are immutable shader metadata. Resolve their
        // packed destinations once instead of scanning every literal for every
        // uniform on every draw. Preserve original override order and bytes.
        for(int n=0;n<result->constant_count;++n){const auto& c=result->constants[n];
            if(c.type!=u.type||c.index<u.index||unsigned(c.index-u.index)>=count)continue;
            UniformLiteral literal{unsigned(uniformBytes)+unsigned(c.index-u.index)*16,c.type==MOJOSHADER_UNIFORM_BOOL?4u:16u,{}};
            std::memcpy(literal.value.data(),&c.value,literal.bytes);uniformLiterals.push_back(literal);
        }
        uniformBytes+=std::size_t(count)*16;
    }
}
unsigned Shader::inputRegister(MOJOSHADER_usage usage, unsigned index) const {
    if(parsed->major_ver<2) {
        const auto reg=legacyInput(bytecode,usage,index);
        if(reg!=UINT32_MAX) return reg;
        throw std::runtime_error("Unsupported legacy stock vertex semantic");
    }
    // DCL tokens carry the register number; declaration order can differ from
    // both semantic order and shader register order. Shader Model 2/3 bytecode
    // has explicit instruction lengths. MojoShader already validated it.
    for(std::size_t at=1;at<bytecode.size();) {
        const auto token=bytecode[at];const auto op=token&65535;
        if(op==65535) break;
        const auto count=op==65534?((token>>16)&32767):((token>>24)&15);
        if(at+count>=bytecode.size()) break;
        if(op==31 && count==2) {
            const auto semantic=bytecode[at+1],reg=bytecode[at+2];
            const auto type=((reg>>28)&7)|((reg>>8)&24);
            if(type==1&&(semantic&15)==unsigned(usage)&&((semantic>>16)&15)==index) return reg&2047;
        }
        at+=count+1;
    }
    throw std::runtime_error("Stock vertex declaration lacks a reflected input register");
}
void Shader::packUniforms(void* destination, std::span<const float> floats,
    std::span<const int> integers, std::span<const int> booleans, bool zeroTail) const {
    auto* output = static_cast<unsigned char*>(destination);
    const auto copyVectors=[zeroTail](void* target, auto values, std::size_t first, std::size_t count, const char* error) {
        if(first+count<=values.size()/4){std::memcpy(target,values.data()+first*4,count*16);return;}
        if(!zeroTail)throw std::runtime_error(error);
        std::memset(target,0,count*16);
        const auto available=first<values.size()/4?(std::min)(count,values.size()/4-first):0;
        if(available)std::memcpy(target,values.data()+first*4,available*16);
    };
    for(const auto& u:uniformCopies){auto* target=output+u.offset;const auto end=std::size_t(u.index)+u.count;
        if(u.type==MOJOSHADER_UNIFORM_FLOAT)copyVectors(target,floats,u.index,u.count,"Vertex/pixel float registers exceeded");
        else if(u.type==MOJOSHADER_UNIFORM_INT)copyVectors(target,integers,u.index,u.count,"Vertex/pixel integer registers exceeded");
        else {if(!zeroTail&&end>booleans.size())throw std::runtime_error("Vertex/pixel boolean registers exceeded");std::memset(target,0,std::size_t(u.count)*16);for(unsigned n=0;n<u.count&&u.index+n<booleans.size();++n)std::memcpy(target+n*16,&booleans[u.index+n],4);}
    }
    for(const auto& literal:uniformLiterals)std::memcpy(output+literal.offset,literal.value.data(),literal.bytes);
}
void validateSpirv(std::span<const std::uint32_t> words, const char* name) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string diagnostics;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& p, const char* text) {
        diagnostics += std::to_string(p.index) + ": " + text + '\n';
    });
    if (!tools.Validate(words.data(), words.size())) throw std::runtime_error(std::string("Invalid Vulkan SPIR-V: ") + name + '\n' + diagnostics);
}
LinkedShaders link(const Shader& vertex, const Shader& pixel, std::span<const MOJOSHADER_vertexAttribute> attributes) {
    LinkedShaders result;
    auto vp = *vertex.parsed; auto pp = *pixel.parsed;
    result.vertex.resize(std::size_t(vp.output_len) / 4); result.pixel.resize(std::size_t(pp.output_len) / 4);
    std::memcpy(result.vertex.data(), vp.output, vp.output_len); std::memcpy(result.pixel.data(), pp.output, pp.output_len);
    vp.output = reinterpret_cast<const char*>(result.vertex.data()); pp.output = reinterpret_cast<const char*>(result.pixel.data());
    const int patchBytes = MOJOSHADER_linkSPIRVShaders(&vp, &pp, attributes.data(), int(attributes.size()));
    if (patchBytes <= 0 || patchBytes % 4 || patchBytes >= vp.output_len || patchBytes >= pp.output_len) throw std::runtime_error("Stock vertex/pixel shader linkage failed");
    result.vertex.resize((vp.output_len - patchBytes) / 4); result.pixel.resize((pp.output_len - patchBytes) / 4);
    if(pixel.parsed->major_ver<2) {
        // MojoShader emits the PS 1.x final r0 register as an Output variable
        // without a Location decoration. D3D defines r0 as COLOR0. Add only
        // that missing interface decoration; all instructions stay unchanged.
        std::unordered_set<std::uint32_t> decorated;
        std::vector<std::uint32_t> outputs;
        std::size_t types=0;
        for(std::size_t at=5;at<result.pixel.size();) {
            const auto count=result.pixel[at]>>16,op=result.pixel[at]&65535;
            if(op==spv::OpDecorate&&(result.pixel[at+2]==spv::DecorationLocation||result.pixel[at+2]==spv::DecorationBuiltIn)) decorated.insert(result.pixel[at+1]);
            if(op==spv::OpVariable&&result.pixel[at+3]==spv::StorageClassOutput) outputs.push_back(result.pixel[at+2]);
            if(!types&&op==spv::OpTypeVoid) types=at;
            at+=count;
        }
        for(auto output:outputs) if(!decorated.contains(output)) {
            if(!types) throw std::runtime_error("Legacy pixel interface has no type section");
            const std::uint32_t location[]={4u<<16|spv::OpDecorate,output,spv::DecorationLocation,0};
            result.pixel.insert(result.pixel.begin()+types,std::begin(location),std::end(location));types+=4;
        }
    }
    integerPixelPosition(result.pixel);
    validateSpirv(result.vertex, vertex.name.c_str()); validateSpirv(result.pixel, pixel.name.c_str());
    return result;
}
}
