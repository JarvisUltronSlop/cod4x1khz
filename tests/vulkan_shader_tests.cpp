#include "gfx_vulkan/vk_shader.h"
#include "graphics_resource_table.hpp"
#include <d3dcompiler.h>
#include <cassert>
#include <cstring>
#include <iostream>

std::vector<std::uint32_t> compile(const char* source, const char* profile) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    const auto result = D3DCompile(source, std::strlen(source), "fixture", nullptr, nullptr,
        "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    std::string diagnostics = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
    if (errors) errors->Release();
    if (FAILED(result)) throw std::runtime_error(diagnostics);
    std::vector<std::uint32_t> words(code->GetBufferSize() / 4);
    std::memcpy(words.data(), code->GetBufferPointer(), code->GetBufferSize()); code->Release(); return words;
}
int main() {
    try {
        gfx::ResourceTable<int, 1> resources;
        const auto old = resources.create(9);
        assert(resources.get(old) == 9 && resources.erase(old));
        assert(!resources.erase(old) && !resources.find(old));
        const auto current = resources.create(10);
        assert(current != old && !resources.find(old));
        bool full = false; try { resources.create(11); } catch (...) { full = true; } assert(full);
        assert(resources.erase(current));
        // Do not alias the first generation after the 16-bit generation is exhausted.
        for (unsigned i = 2; i < 65535; ++i) assert(resources.erase(resources.create(int(i))));
        full = false; try { resources.create(0); } catch (...) { full = true; } assert(full);
        const char* vs = "float4 transform[4]:register(c4); struct I{float4 p:POSITION;float2 t:TEXCOORD0;};"
            "struct O{float4 p:POSITION;float2 t:TEXCOORD0;}; O main(I i){O o;o.p=mul(i.p,float4x4(transform[0],transform[1],transform[2],transform[3]));o.t=i.t;return o;}";
        const char* ps = "sampler2D image:register(s0);float4 tint:register(c7);float4 main(float2 t:TEXCOORD0):COLOR0{return tex2D(image,t)*tint;}";
        const auto vcode = compile(vs, "vs_3_0"), pcode = compile(ps, "ps_3_0");
        gfx::vk::Shader vertex(vcode, "fixture VS3"), pixel(pcode, "fixture PS3");
        const MOJOSHADER_vertexAttribute attributes[] = {
            {MOJOSHADER_USAGE_POSITION, MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR4, 0},
            {MOJOSHADER_USAGE_TEXCOORD, MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR2, 0}};
        const auto linked = gfx::vk::link(vertex, pixel, attributes);
        assert(linked.vertex[0] == 0x07230203 && linked.pixel[0] == 0x07230203);
        const auto again = gfx::vk::link(vertex, pixel, attributes);
        assert(again.vertex == linked.vertex && again.pixel == linked.pixel);
        assert(vertex.inputRegister(MOJOSHADER_USAGE_POSITION,0)==0 && vertex.inputRegister(MOJOSHADER_USAGE_TEXCOORD,0)==1);
        for(unsigned comparison=0;comparison<8;++comparison) {
            assert(!gfx::vk::alphaTest(linked.pixel,comparison).empty());
            assert(!gfx::vk::fragmentState(linked.pixel,int(comparison),true,true).empty());
        }
        assert(!gfx::vk::fragmentState(linked.pixel,-1,true,false).empty());
        // Project-authored SM1.1 programs reproduce the legacy stock path
        // without distributing any retail bytecode in the regression suite.
        const std::uint32_t legacyVs[]={0xfffe0101,31,0x80000000,0x900f0000,31,0x80000005,0x900f0007,1,0xc00f0000,0x90e40000,1,0xe00f0000,0x90e40007,0xffff};
        const std::uint32_t legacyPs[]={0xffff0101,66,0xb00f0000,1,0x800f0000,0xb0e40000,0xffff};
        gfx::vk::Shader lv(legacyVs,"authored VS1.1");
        gfx::vk::Shader lp(legacyPs,"authored PS1.1");
        assert(lv.inputRegister(MOJOSHADER_USAGE_TEXCOORD,0)==7);
        const auto legacy=gfx::vk::link(lv,lp,attributes);assert(legacy.vertex[0]==0x07230203&&legacy.pixel[0]==0x07230203);
        bool missingDeclaration=false;
        try {const std::uint32_t undeclared[]={0xfffe0101,1,0xc00f0000,0x90e40000,0xffff};gfx::vk::Shader invalid(undeclared,"undeclared legacy input");}
        catch(const std::exception&) {missingDeclaration=true;} assert(missingDeclaration);
        std::array<float, 256 * 4> floats{}; std::array<int, 16 * 4> integers{}; std::array<int, 16> booleans{};
        floats[28] = .25f; floats[29] = .5f; floats[30] = .75f; floats[31] = 1;
        std::vector<float> packed(pixel.uniformBytes / 4);
        pixel.packUniforms(packed.data(), floats, integers, booleans);
        assert(packed.size() == 4 && packed[0] == .25f && packed[3] == 1);
        // The legacy packet's omitted tail represents only never-written
        // registers. Ordinary backend callers still reject undersized spans.
        bool shortRejected=false;
        try {pixel.packUniforms(packed.data(), {}, {}, {});}
        catch(const std::exception&) {shortRejected=true;}
        assert(shortRejected);
        std::fill(packed.begin(),packed.end(),9.0f);
        pixel.packUniforms(packed.data(), {}, {}, {}, true);
        for(auto value:packed)assert(value==0);
        std::vector<float> transform(vertex.uniformBytes/4,9.0f);
        for(unsigned n=16;n<28;++n)floats[n]=float(n);
        vertex.packUniforms(transform.data(), std::span<const float>(floats).first(28), {}, {}, true);
        assert(transform.size()==16);
        for(unsigned n=0;n<12;++n)assert(transform[n]==float(n+16));
        for(unsigned n=12;n<16;++n)assert(transform[n]==0);
        // Authored VS3 REP reads a sparse integer loop register. HLSL maps
        // general int uniforms to float constants, so use actual DX9 tokens.
        const std::uint32_t sparseIntVs[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,
            0x0200001f,0x80000000,0xe00f0000,0x02000001,0x800f0000,0x90e40000,
            0x01000026,0xf0e40009,0x03000002,0x800f0000,0x80e40000,0xa0e4000c,
            0x00000027,0x02000001,0xe00f0000,0x80e40000,0xffff};
        gfx::vk::Shader sparseInts(sparseIntVs,"sparse integer VS3");
        std::vector<std::byte> packedInts(sparseInts.uniformBytes);
        sparseInts.packUniforms(packedInts.data(), {}, {}, {}, true);
        for(auto value:packedInts)assert(value==std::byte{});
        integers[36]=2;integers[37]=3;integers[38]=4;integers[39]=5;
        sparseInts.packUniforms(packedInts.data(), {}, std::span<const int>(integers).first(40), {}, true);
        bool foundInt=false;unsigned offset=0;
        for(int n=0;n<sparseInts.parsed->uniform_count;++n){const auto& u=sparseInts.parsed->uniforms[n];
            if(u.type==MOJOSHADER_UNIFORM_INT){assert(u.index==9);assert(std::memcmp(packedInts.data()+offset,integers.data()+36,16)==0);foundInt=true;}
            offset+=unsigned((std::max)(1,u.array_count))*16;
        }
        assert(foundInt);
        bool rejected = false; try { gfx::vk::Shader bad(std::array<std::uint32_t, 3>{0, 1, 2}, "invalid"); } catch (...) { rejected = true; } assert(rejected);
        std::cout << "PASS: bounded generation handles, SM3 linking, immutable translation, constant register packing, invalid-bytecode rejection\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
