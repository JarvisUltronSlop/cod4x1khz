#include "gfx_vulkan/vk_backend.h"
#include "gfx/gamma_pass.h"
#include "gfx_d3d/d3d9_backend.h"
#include <d3dcompiler.h>
#include <d3dx9.h>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
std::vector<std::uint32_t> compile(const char* source,const char* profile) {
    ID3DBlob *code=nullptr,*errors=nullptr;
    const auto result=D3DCompile(source,std::strlen(source),"GPU parity fixture",nullptr,nullptr,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    const std::string diagnostic=errors?std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"";
    if(errors) errors->Release(); if(FAILED(result)) throw std::runtime_error(diagnostic);
    std::vector<std::uint32_t> words(code->GetBufferSize()/4); std::memcpy(words.data(),code->GetBufferPointer(),code->GetBufferSize()); code->Release(); return words;
}
void capture(const std::filesystem::path& path,const std::vector<std::byte>& pixels) {
    std::ofstream out(path,std::ios::binary); out<<"P6\n64 64\n255\n";
    for(std::size_t i=0;i<pixels.size();i+=4) { const char rgb[]={char(pixels[i+2]),char(pixels[i+1]),char(pixels[i])}; out.write(rgb,3); }
    if(!out) throw std::runtime_error("Fixture capture write failed");
}
std::vector<std::uint32_t> assemble(const char* source) {
    ID3DXBuffer *code=nullptr,*errors=nullptr;
    const auto result=D3DXAssembleShader(source,unsigned(std::strlen(source)),nullptr,nullptr,0,&code,&errors);
    const std::string diagnostic=errors?std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"";
    if(errors)errors->Release();if(FAILED(result))throw std::runtime_error(diagnostic);
    std::vector<std::uint32_t> words(code->GetBufferSize()/4);std::memcpy(words.data(),code->GetBufferPointer(),code->GetBufferSize());code->Release();return words;
}
struct Fixture {
    gfx::ResourceId target,texture,vertices,indices,vertex,pixel;
    std::array<gfx::VertexElement,2> elements{{{0,0,0,gfx::VertexFormat::Float4,gfx::Semantic::Position},{0,16,0,gfx::VertexFormat::Float2,gfx::Semantic::Texcoord}}};
    std::array<float,256*4> vc{}; std::array<float,224*4> pc{};
    std::array<int,16*4> integers{}; std::array<int,16> booleans{};
    gfx::DrawPacket packet;
    explicit Fixture(gfx::Backend& b) {
        gfx::ImageDesc desc; desc.width=desc.height=64; desc.renderTarget=true; target=b.createImage(desc);
        desc.width=desc.height=8; desc.renderTarget=false; texture=b.createImage(desc);
        const float geometry[][6]={{-.8f,.8f,.5f,1,0,0},{.8f,.8f,.5f,1,1,0},{.8f,-.8f,.5f,1,1,1},{-.8f,-.8f,.5f,1,0,1}};
        const std::uint16_t index[]={0,1,2,0,2,3};
        vertices=b.createBuffer(std::as_bytes(std::span(geometry)),false); indices=b.createBuffer(std::as_bytes(std::span(index)),true);
        const auto vs=compile("struct I{float4 p:POSITION;float2 t:TEXCOORD0;};struct O{float4 p:POSITION;float2 t:TEXCOORD0;};float4 shift:register(c4);O main(I i){O o;o.p=i.p+shift;o.t=i.t;return o;}","vs_3_0");
        const auto ps=compile("sampler2D image:register(s0);float4 tint:register(c7);float4 main(float2 t:TEXCOORD0):COLOR0{return tex2D(image,t)*tint;}","ps_3_0");
        vertex=b.createShader(vs,"GPU fixture VS3"); pixel=b.createShader(ps,"GPU fixture PS3");
        std::vector<std::byte> texels(8*8*4);
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) { const auto p=(y*8+x)*4; texels[p]=std::byte(30+x*20); texels[p+1]=std::byte(40+y*20); texels[p+2]=std::byte(200); texels[p+3]=std::byte(255); }
        b.beginFrame(); b.uploadImage(texture,0,0,texels); b.endFrame({});
        pc[28]=pc[29]=pc[30]=pc[31]=1;
        packet.vertexShader=vertex; packet.pixelShader=pixel; packet.indexBuffer=indices; packet.vertexBuffers[0]=vertices; packet.strides[0]=24; packet.textures[0]=texture; packet.elements=elements;
        packet.vertexConstants=vc; packet.pixelConstants=pc; packet.vertexIntegers=packet.pixelIntegers=integers; packet.vertexBooleans=packet.pixelBooleans=booleans;
        packet.state.depthTest=packet.state.depthWrite=packet.state.cull=false; packet.colorTarget=target; packet.width=packet.height=64; packet.indexCount=6;
        packet.samplers[0].u=packet.samplers[0].v=gfx::Address::Clamp;
    }
    void draw(gfx::Backend& b,bool present) {
        b.warm(packet); b.beginFrame(); b.clear(target,{}, {.1f,.2f,.3f,1},1); b.draw(packet); b.endFrame(present?target:gfx::ResourceId{});
    }
    void release(gfx::Backend& b) { for(auto id:{target,texture,vertices,indices,vertex,pixel}) b.release(id); }
};
LRESULT CALLBACK windowProc(HWND h,UINT m,WPARAM w,LPARAM l) { return DefWindowProcW(h,m,w,l); }
std::uint64_t queryCases(gfx::Backend& b,Fixture& f) {
    const auto samples=b.createQuery(gfx::QueryType::Occlusion),event=b.createQuery(gfx::QueryType::Event),stamp=b.createQuery(gfx::QueryType::Timestamp);
    std::uint64_t value=0;assert(!b.queryResult(samples,value));
    b.warm(f.packet);b.beginFrame();b.clear(f.target,{}, {.1f,.2f,.3f,1},1);b.beginQuery(samples);b.draw(f.packet);b.draw(f.packet);b.endQuery(samples);b.endQuery(stamp);b.endQuery(event);
    if(b.api()==gfx::Api::Vulkan) assert(!b.queryResult(event,value));
    b.endFrame({});b.waitIdle();assert(b.queryResult(event,value)&&value==1);assert(b.queryResult(stamp,value)&&value>0);assert(b.queryResult(samples,value)&&value>0);const auto count=value;
    b.beginFrame();b.beginQuery(samples);b.endQuery(samples);b.endFrame({});b.waitIdle();assert(b.queryResult(samples,value)&&value==0);
    for(auto id:{samples,event,stamp}) b.release(id);return count;
}
std::vector<std::vector<std::byte>> materialCases(gfx::Backend& backend,Fixture& f) {
    std::vector<std::vector<std::byte>> captures;
    // Reordering a declaration must not reorder shader input registers.
    std::swap(f.elements[0],f.elements[1]);
    f.draw(backend,false);captures.push_back(backend.readback(f.target));std::swap(f.elements[0],f.elements[1]);
    f.packet.state.alphaTest=true;f.packet.state.alphaCompare=gfx::Compare::GreaterEqual;f.packet.state.alphaRef=128;
    for(float alpha:{.25f,.75f}) {f.pc[31]=alpha;f.draw(backend,false);captures.push_back(backend.readback(f.target));}
    f.packet.state.alphaTest=false;f.packet.state.blend=true;f.packet.state.src=gfx::Blend::SrcAlpha;f.packet.state.dst=gfx::Blend::InvSrcAlpha;
    f.pc[31]=.5f;f.draw(backend,false);captures.push_back(backend.readback(f.target));
    f.packet.state.blend=false;f.pc[31]=1;
    gfx::ImageDesc depth;depth.width=depth.height=64;depth.format=gfx::ImageFormat::DepthStencil;depth.renderTarget=true;depth.sampled=false;
    const auto depthId=backend.createImage(depth);f.packet.depthTarget=depthId;f.packet.state.depthTest=f.packet.state.depthWrite=true;
    for(auto comparison:{gfx::Compare::Less,gfx::Compare::Greater}) {
        f.packet.state.depthCompare=comparison;backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,depthId,{.1f,.2f,.3f,1},1);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
    }
    // Stock bias is an offset in normalized depth units, even when Vulkan
    // uses D32S8 because the device does not support native D24S8.
    f.packet.state.depthCompare=gfx::Compare::Less;
    for(float bias:{-.08f,0.f,.08f}) {
        f.packet.state.depthBias=bias;backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,depthId,{.1f,.2f,.3f,1},.5f);
        backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
        const auto center=(32*64+32)*4;assert(unsigned(captures.back()[center+2])==(bias<0?200u:26u));
    }
    f.packet.state.depthBias=0;
    f.packet.state.depthBias=-.08f;f.packet.state.alphaTest=true;f.packet.state.alphaCompare=gfx::Compare::GreaterEqual;f.packet.state.alphaRef=128;
    for(float alpha:{.25f,.75f}) {
        f.pc[31]=alpha;backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,depthId,{.1f,.2f,.3f,1},.5f);
        backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
    }
    f.packet.state.alphaTest=false;f.pc[31]=1;
    {
        const auto code=compile("sampler2D image:register(s0);struct O{float4 c:COLOR0;float d:DEPTH;};O main(float2 t:TEXCOORD0){O o;o.c=tex2D(image,t);o.d=.6;return o;}","ps_3_0");
        const auto shader=backend.createShader(code,"explicit pixel depth overrides rasterizer bias");f.packet.pixelShader=shader;f.packet.state.depthBias=-.2f;
        backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,depthId,{.1f,.2f,.3f,1},.5f);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
        assert(unsigned(captures.back()[(32*64+32)*4+2])==26);f.packet.pixelShader=f.pixel;backend.release(shader);
    }
    f.packet.state.depthBias=0;
    {
        const float sloped[][6]={{-.8f,.8f,.25f,1,0,0},{.8f,.8f,.75f,1,1,0},{.8f,-.8f,.75f,1,1,1},{-.8f,-.8f,.25f,1,0,1}};
        const auto vertices=backend.createBuffer(std::as_bytes(std::span(sloped)),false,false);f.packet.vertexBuffers[0]=vertices;
        for(float slope:{-2.f,2.f}) {
            f.packet.state.slopeDepthBias=slope;backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,depthId,{.1f,.2f,.3f,1},.5f);
            backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
        }
        f.packet.state.slopeDepthBias=0;f.packet.vertexBuffers[0]=f.vertices;backend.release(vertices);
    }
    f.packet.state.depthCompare=gfx::Compare::LessEqual;f.packet.state.stencil=true;f.packet.state.front.compare=f.packet.state.back.compare=gfx::Compare::Equal;
    for(unsigned ref:{0u,1u}) {
        f.packet.state.stencilRef=ref;backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,depthId,{.1f,.2f,.3f,1},1);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
    }
    f.packet.state.stencil=f.packet.state.depthTest=f.packet.state.depthWrite=false;f.packet.depthTarget={};backend.release(depthId);
    {
        auto shadow=depth;shadow.sampled=true;const auto image=backend.createImage(shadow);
        const auto plain=assemble("ps_3_0\ndcl_texcoord v0.xy\ndcl_2d s0\ndef c0,0,0,0.5,1\nmov r0,c0\nmov r0.xy,v0\ntexld r1,r0,s0\nmov oC0,r1\n");
        const auto projected=compile("sampler2D image:register(s0);float4 main(float2 t:TEXCOORD0):COLOR0{return tex2Dproj(image,float4(t*2,1,2));}","ps_3_0");
        for(const auto& code:{plain,projected}) {
            const auto shader=backend.createShader(code,"stock hardware depth sampling");f.packet.pixelShader=shader;f.packet.textures[0]=image;f.packet.samplers[0].compare=true;
            for(float z:{.4f,.6f}) {
                backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,image,{.1f,.2f,.3f,1},z);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
                const auto actual=unsigned(captures.back()[(32*64+32)*4+2]);if(actual!=(z<.5f?0u:255u))throw std::runtime_error("Shadow result API="+std::to_string(unsigned(backend.api()))+" depth="+std::to_string(z)+" red="+std::to_string(actual));
            }
            const std::array<gfx::ClearRect,1> right{{{32,0,64,64}}};
            backend.beginFrame();backend.clear(f.target,image,{.1f,.2f,.3f,1},.4f);backend.clear({},image,{},.6f,0,true,false,right);
            backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
            backend.release(shader);
        }
        f.packet.pixelShader=f.pixel;f.packet.textures[0]=f.texture;f.packet.samplers[0].compare=false;backend.release(image);
    }
    {
        auto oversized=depth;oversized.width=oversized.height=128;const auto id=backend.createImage(oversized);
        f.packet.depthTarget=id;f.packet.state.depthTest=f.packet.state.depthWrite=true;f.packet.state.depthCompare=gfx::Compare::Less;
        backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,id,{.1f,.2f,.3f,1},1);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
        f.packet.depthTarget={};f.packet.state.depthTest=f.packet.state.depthWrite=false;backend.release(id);
    }
    // A dynamic update after one draw must preserve that draw's uploaded vertex
    // bytes until GPU completion. The later draw uses a separate arena slice.
    const float original[][6]={{-.8f,.8f,.5f,1,0,0},{0,.8f,.5f,1,1,0},{0,-.8f,.5f,1,1,1},{-.8f,-.8f,.5f,1,0,1}};
    const float changed[][6]={{0,.8f,.5f,1,0,0},{.8f,.8f,.5f,1,1,0},{.8f,-.8f,.5f,1,1,1},{0,-.8f,.5f,1,0,1}};
    const auto dynamic=backend.createBuffer(std::as_bytes(std::span(original)),false,true);f.packet.vertexBuffers[0]=dynamic;
    backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,{}, {.1f,.2f,.3f,1},1);backend.draw(f.packet);backend.updateBuffer(dynamic,0,std::as_bytes(std::span(changed)));backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
    f.packet.vertexBuffers[0]=f.vertices;backend.release(dynamic);
    {
        std::array<std::byte,sizeof(original)*2> zeros{};const auto appended=backend.createBuffer(zeros,false,true);
        f.packet.vertexBuffers[0]=appended;f.packet.vertexCount=4;
        backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,{}, {.1f,.2f,.3f,1},1);
        backend.updateBuffer(appended,0,std::as_bytes(std::span(original)),true);backend.draw(f.packet);
        if(backend.api()==gfx::Api::Vulkan){bool rejected=false;try{backend.updateBuffer(appended,0,std::as_bytes(std::span(changed)),false,true);}catch(const std::exception&){rejected=true;}assert(rejected);}
        backend.updateBuffer(appended,sizeof(original),std::as_bytes(std::span(changed)),false,true);
        f.packet.vertexOffsets[0]=sizeof(original);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
        f.packet.vertexBuffers[0]=f.vertices;f.packet.vertexOffsets[0]=0;f.packet.vertexCount=0;backend.release(appended);
    }
    // Fixed legacy luminance/alpha swizzles must match D3D9's sampling behavior.
    for(auto format:{gfx::ImageFormat::R8,gfx::ImageFormat::Rg8,gfx::ImageFormat::Alpha8}) {
        gfx::ImageDesc desc;desc.width=desc.height=8;desc.format=format;const auto texture=backend.createImage(desc);const unsigned bytes=format==gfx::ImageFormat::Rg8?128:64;
        std::vector<std::byte> texels(bytes,std::byte(128));backend.beginFrame();backend.uploadImage(texture,0,0,texels);backend.endFrame({});f.packet.textures[0]=texture;f.draw(backend,false);captures.push_back(backend.readback(f.target));backend.release(texture);
    }
    f.packet.textures[0]=f.texture;
    // 4x MSAA is rendered and explicitly resolved before readback/presentation.
    gfx::ImageDesc msaa;msaa.width=msaa.height=64;msaa.renderTarget=true;msaa.sampled=false;msaa.samples=4;
    const auto multisample=backend.createImage(msaa);f.packet.colorTarget=multisample;backend.warm(f.packet);backend.beginFrame();backend.clear(multisample,{}, {.1f,.2f,.3f,1},1);backend.draw(f.packet);backend.resolve(multisample,f.target);backend.endFrame({});captures.push_back(backend.readback(f.target));
    f.packet.colorTarget=f.target;backend.release(multisample);
    const std::uint32_t legacyVs[]={0xfffe0101,31,0x80000000,0x900f0000,31,0x80000005,0x900f0007,1,0xc00f0000,0x90e40000,1,0xe00f0000,0x90e40007,0xffff};
    const std::uint32_t legacyPs[]={0xffff0101,66,0xb00f0000,1,0x800f0000,0xb0e40000,0xffff};
    const auto lv=backend.createShader(legacyVs,"authored VS1.1"),lp=backend.createShader(legacyPs,"authored PS1.1");
    f.packet.vertexShader=lv;f.packet.pixelShader=lp;f.draw(backend,false);captures.push_back(backend.readback(f.target));f.packet.vertexShader=f.vertex;f.packet.pixelShader=f.pixel;backend.release(lv);backend.release(lp);
    {
        gfx::GammaPass gamma(backend);
        assert(gamma.identity());
        gamma.prepare(f.target,64,64);backend.beginFrame();const auto output=gamma.draw();backend.endFrame({});
        const auto unchanged=backend.readback(output);assert(unchanged==captures.back());captures.push_back(unchanged);
        auto ramp=gamma.getRamp();
        for(unsigned n=0;n<256;++n){ramp[0][n]=std::uint16_t(n*n*65535u/(255u*255u));ramp[1][n]=65535-ramp[1][n];}
        gamma.setRamp(ramp);assert(!gamma.identity());gamma.prepare(f.target,64,64);
        backend.beginFrame();const auto corrected=gamma.draw();backend.endFrame({});const auto pixels=backend.readback(corrected);
        for(std::size_t n=0;n<pixels.size();n+=4) {
            const auto red=unsigned(unchanged[n+2]),green=unsigned(unchanged[n+1]);
            const auto expected=(unsigned(ramp[0][red])*255+32767)/65535;
            assert(std::abs(int(pixels[n+2])-int(expected))<=1);
            assert(std::abs(int(pixels[n+1])-int(255-green))<=1);
            assert(pixels[n]==unchanged[n]&&pixels[n+3]==unchanged[n+3]);
        }
        captures.push_back(pixels);
        // A later presentation-size change replaces only the gamma output.
        gamma.prepare(f.target,32,32);backend.beginFrame();const auto resized=gamma.draw();backend.endFrame({});assert(backend.readback(resized).size()==32*32*4);
    }
    {
        const auto depthSurface=backend.createImage(depth);
        f.packet.depthTarget=depthSurface;f.packet.state.stencil=true;f.packet.state.stencilRef=7;
        f.packet.state.front.compare=f.packet.state.back.compare=gfx::Compare::Equal;
        f.packet.state.depthTest=f.packet.state.depthWrite=true;f.packet.state.depthCompare=gfx::Compare::Less;
        backend.warm(f.packet);backend.beginFrame();backend.clear(f.target,depthSurface,{.1f,.2f,.3f,1},0,7);
        backend.clear({},depthSurface,{},.75f,0,true,false);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
        const std::array<gfx::ClearRect,1> right{{{32,0,64,64}}};f.packet.state.stencilRef=8;backend.warm(f.packet);
        backend.beginFrame();backend.clear(f.target,{}, {.1f,.2f,.3f,1},1);
        backend.clear({},depthSurface,{},0,8,false,true,right);backend.draw(f.packet);backend.endFrame({});captures.push_back(backend.readback(f.target));
        const std::array<gfx::ClearRect,1> corner{{{0,0,8,8}}};
        backend.beginFrame();backend.clear(f.target,{}, {1,0,0,1},1,0,false,false,corner);backend.endFrame({});captures.push_back(backend.readback(f.target));
        f.packet.depthTarget={};f.packet.state.stencil=f.packet.state.depthTest=f.packet.state.depthWrite=false;backend.release(depthSurface);
    }
    {
        gfx::ImageDesc image;image.width=image.height=64;image.renderTarget=true;image.sampled=false;
        const auto destination=backend.createImage(image);
        backend.beginFrame();backend.blit(f.target,destination);backend.endFrame({});assert(backend.readback(destination)==captures.back());
        const gfx::ClearRect source{8,8,40,40},target{24,24,56,56};
        backend.beginFrame();backend.clear(destination,{}, {.2f,.3f,.4f,1},1);backend.blit(f.target,destination,false,&source,&target);backend.endFrame({});captures.push_back(backend.readback(destination));
        for(bool linear:{false,true}){backend.beginFrame();backend.blit(f.target,destination,linear,&source);backend.endFrame({});captures.push_back(backend.readback(destination));}
        backend.release(destination);
    }
    return captures;
}
struct Window {
    HWND hwnd=nullptr;
    Window(bool visible) {
        WNDCLASSW c{}; c.lpfnWndProc=windowProc; c.hInstance=GetModuleHandleW(nullptr); c.lpszClassName=L"cod4x1khz Vulkan regression"; RegisterClassW(&c);
        hwnd=CreateWindowW(c.lpszClassName,L"cod4x1khz graphics regression",WS_OVERLAPPEDWINDOW,100,100,320,240,nullptr,nullptr,c.hInstance,nullptr);
        if(!hwnd) throw std::runtime_error("Graphics regression window creation failed"); if(visible) ShowWindow(hwnd,SW_SHOWNOACTIVATE);
    }
    ~Window() { if(hwnd) DestroyWindow(hwnd); UnregisterClassW(L"cod4x1khz Vulkan regression",GetModuleHandleW(nullptr)); }
};
}
int main(int argc,char** argv) {
    bool validation=false,present=false; std::filesystem::path output="results/vulkan-gpu";
    for(int n=1;n<argc;++n) { if(!std::strcmp(argv[n],"--validation")) validation=true; else if(!std::strcmp(argv[n],"--present")) present=true; else if(!std::strcmp(argv[n],"--output")&&n+1<argc) output=argv[++n]; else return 2; }
    try {
        std::filesystem::create_directories(output); Window window(present);
        std::vector<std::byte> dxPixels,vkPixels;
        std::vector<std::vector<std::byte>> dxMaterials;
        std::uint64_t dxSampleCount=0;
        {
            gfx::Config config; config.window=window.hwnd; gfx::d3d9::Backend backend(config); Fixture f(backend); f.draw(backend,false); dxPixels=backend.readback(f.target); capture(output/"d3d9.ppm",dxPixels);dxSampleCount=queryCases(backend,f);dxMaterials=materialCases(backend,f);f.release(backend);
        }
        for(unsigned flight:{1u,2u}) {
            gfx::Config config; config.window=present?window.hwnd:nullptr; config.validation=validation; config.framesInFlight=flight; config.cacheDirectory=(output/"cache").string();
            gfx::vk::Backend backend(config); Fixture f(backend);
            for(unsigned n=0;n<32;++n) {
                if(present) {
                    if(n==8) ShowWindow(window.hwnd,SW_MINIMIZE);
                    if(n==12) { ShowWindow(window.hwnd,SW_SHOWNOACTIVATE); SetWindowPos(window.hwnd,nullptr,0,0,400,300,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE); }
                }
                f.draw(backend,present); MSG msg; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            }
            vkPixels=backend.readback(f.target); capture(output/("vulkan-"+std::to_string(flight)+".ppm"),vkPixels);
            const auto center=(32*64+32)*4; assert(unsigned(vkPixels[center+2])==200&&unsigned(vkPixels[center+3])==255); assert(vkPixels.size()==dxPixels.size());
            const auto vkMaterials=materialCases(backend,f);
            assert(queryCases(backend,f)==dxSampleCount);
            for(std::size_t m=0;m<vkMaterials.size();++m) {
                capture(output/("d3d9-material-"+std::to_string(m)+".ppm"),dxMaterials[m]);capture(output/("vulkan-"+std::to_string(flight)+"-material-"+std::to_string(m)+".ppm"),vkMaterials[m]);
                unsigned maxError=0;for(std::size_t p=0;p<vkMaterials[m].size();++p) maxError=(std::max)(maxError,unsigned(std::abs(int(vkMaterials[m][p])-int(dxMaterials[m][p]))));
                if(maxError>1) throw std::runtime_error("Material parity error case "+std::to_string(m)+" max "+std::to_string(maxError));
            }
            const auto old=f.texture; backend.release(old); gfx::ImageDesc d; const auto replacement=backend.createImage(d); assert(old!=replacement);
            backend.beginFrame(); bool rejected=false; try { backend.draw(f.packet); } catch(const std::exception&) { rejected=true; } assert(rejected); backend.endFrame({});
            backend.release(replacement); for(auto id:{f.target,f.vertices,f.indices,f.vertex,f.pixel}) backend.release(id); backend.waitIdle(); assert(backend.validationErrors()==0);
            // A failed, unsubmitted recording must not create a timeline value
            // that shutdown waits for forever. The backend then fails closed.
            backend.beginFrame();backend.cancelFrame();backend.waitIdle();bool failedClosed=false;
            try{backend.beginFrame();}catch(const std::exception&){failedClosed=true;}assert(failedClosed);
            std::ofstream report(output/("device-"+std::to_string(flight)+".json")); report<<backend.deviceReport()<<'\n';
        }
        std::size_t different=0; unsigned maximum=0; double mse=0;
        for(std::size_t n=0;n<dxPixels.size();++n) { const auto error=std::abs(int(dxPixels[n])-int(vkPixels[n])); different+=error!=0; maximum=(std::max)(maximum,unsigned(error)); mse+=double(error*error); } mse/=dxPixels.size();
        std::cout<<"GPU draw/readback and retirement passed; differing channels="<<different<<" max="<<maximum<<" mse="<<mse<<" (image parity evaluated separately)\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
