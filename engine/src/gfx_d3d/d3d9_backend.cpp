#include "d3d9_backend.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <bit>

namespace gfx::d3d9 {
namespace {
void check(HRESULT r, const char* op) { if (FAILED(r)) throw std::runtime_error(std::string(op) + ": HRESULT " + std::to_string(std::uint32_t(r))); }
template<class T> void drop(T*& p) { if (p) p->Release(); p = nullptr; }
D3DFORMAT format(ImageFormat f) {
    switch (f) {
    case ImageFormat::Bgra8: return D3DFMT_A8R8G8B8;
    case ImageFormat::Rgba8: return D3DFMT_A8B8G8R8;
    case ImageFormat::Bc1: return D3DFMT_DXT1; case ImageFormat::Bc2: return D3DFMT_DXT3; case ImageFormat::Bc3: return D3DFMT_DXT5;
    case ImageFormat::R8: return D3DFMT_L8; case ImageFormat::Rg8: return D3DFMT_A8L8; case ImageFormat::R32Float: return D3DFMT_R32F;
    case ImageFormat::DepthStencil: return D3DFMT_D24S8;
    case ImageFormat::Alpha8: return D3DFMT_A8;
    case ImageFormat::Rgba16Unorm: return D3DFMT_A16B16G16R16;
    case ImageFormat::Rgba16Float: return D3DFMT_A16B16G16R16F;
    } throw std::runtime_error("Unsupported image format");
}
struct Resource {
    IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DTexture9* texture = nullptr; IDirect3DCubeTexture9* cube = nullptr; IDirect3DVolumeTexture9* volume = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
    IDirect3DQuery9* query = nullptr; QueryType queryType = QueryType::Event;bool queryIssued=false;
    ImageDesc desc; unsigned bytes = 0;bool dynamic=false;
    IDirect3DBaseTexture9* base() { if (texture) return texture; if (cube) return cube; return volume; }
    ~Resource() { drop(query);drop(vb); drop(ib); drop(surface); drop(texture); drop(cube); drop(volume); drop(vs); drop(ps); }
};
const D3DBLEND blendMap[] = {D3DBLEND_ZERO,D3DBLEND_ONE,D3DBLEND_SRCCOLOR,D3DBLEND_INVSRCCOLOR,D3DBLEND_SRCALPHA,D3DBLEND_INVSRCALPHA,D3DBLEND_DESTALPHA,D3DBLEND_INVDESTALPHA,D3DBLEND_DESTCOLOR,D3DBLEND_INVDESTCOLOR,D3DBLEND_SRCALPHASAT};
const D3DTEXTUREADDRESS addressMap[] = {D3DTADDRESS_WRAP,D3DTADDRESS_MIRROR,D3DTADDRESS_CLAMP,D3DTADDRESS_BORDER};
const D3DDECLTYPE vertexMap[] = {D3DDECLTYPE_FLOAT1,D3DDECLTYPE_FLOAT2,D3DDECLTYPE_FLOAT3,D3DDECLTYPE_FLOAT4,D3DDECLTYPE_D3DCOLOR,D3DDECLTYPE_UBYTE4,D3DDECLTYPE_SHORT2,D3DDECLTYPE_SHORT4,D3DDECLTYPE_SHORT2N,D3DDECLTYPE_SHORT4N,D3DDECLTYPE_FLOAT16_2,D3DDECLTYPE_FLOAT16_4,D3DDECLTYPE_UBYTE4N,D3DDECLTYPE_USHORT2N,D3DDECLTYPE_USHORT4N,D3DDECLTYPE_UDEC3,D3DDECLTYPE_DEC3N};
}
struct Backend::Impl {
    ResourceTable<Resource,16384> resources;
    IDirect3D9* api = nullptr; IDirect3DDevice9* device = nullptr;
    std::thread::id owner = std::this_thread::get_id(); bool frame = false;
    D3DADAPTER_IDENTIFIER9 identifier{}; FrameTiming timing; std::chrono::steady_clock::time_point start;
    explicit Impl(const Config& c) {
        api = Direct3DCreate9(D3D_SDK_VERSION); if (!api) throw std::runtime_error("D3D9 unavailable");
        try {
            const unsigned adapter = c.device < 0 ? D3DADAPTER_DEFAULT : unsigned(c.device);
            check(api->GetAdapterIdentifier(adapter, 0, &identifier), "DX9 adapter identity");
            D3DPRESENT_PARAMETERS pp{}; pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = static_cast<HWND>(c.window); pp.Windowed = TRUE; pp.PresentationInterval = c.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
            check(api->CreateDevice(adapter, D3DDEVTYPE_HAL, pp.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device), "DX9 comparison device");
        } catch (...) { drop(api); throw; }
    }
    void assertOwner() { if (owner != std::this_thread::get_id()) throw std::runtime_error("D3D9 owner violated"); }
    void targets(ResourceId color, ResourceId depth) { check(device->SetRenderTarget(0, color ? resources.get(color).surface : nullptr), "DX9 target"); check(device->SetDepthStencilSurface(depth ? resources.get(depth).surface : nullptr), "DX9 depth target"); }
    ~Impl() { resources.forEach([](Resource& r) { /* resource destructors run below */ }); drop(device); drop(api); }
};
Backend::Backend(const Config& c) : impl(std::make_unique<Impl>(c)) {}
Backend::~Backend() = default;
ResourceId Backend::createQuery(QueryType type) {
    auto& i=*impl;i.assertOwner();auto id=i.resources.create();auto& r=i.resources.get(id);r.queryType=type;
    const D3DQUERYTYPE map[]={D3DQUERYTYPE_EVENT,D3DQUERYTYPE_OCCLUSION,D3DQUERYTYPE_TIMESTAMP};
    try {check(i.device->CreateQuery(map[unsigned(type)],&r.query),"DX9 query creation");}catch(...){i.resources.erase(id);throw;}return id;
}
void Backend::beginQuery(ResourceId id) {auto& i=*impl;i.assertOwner();auto& r=i.resources.get(id);if(!r.query||r.queryType!=QueryType::Occlusion) throw std::runtime_error("Invalid DX9 query begin");check(r.query->Issue(D3DISSUE_BEGIN),"DX9 query begin");r.queryIssued=false;}
void Backend::endQuery(ResourceId id) {auto& i=*impl;i.assertOwner();auto& r=i.resources.get(id);if(!r.query) throw std::runtime_error("Invalid DX9 query end");check(r.query->Issue(D3DISSUE_END),"DX9 query end");r.queryIssued=true;}
bool Backend::queryResult(ResourceId id,std::uint64_t& value) {
    auto& i=*impl;i.assertOwner();auto& r=i.resources.get(id);if(!r.query) throw std::runtime_error("Invalid DX9 query result");
    if(!r.queryIssued) return false;std::uint64_t raw=0;const auto result=r.query->GetData(&raw,r.queryType==QueryType::Timestamp?8:4,0);
    if(result==S_FALSE) return false;check(result,"DX9 query result");value=raw;return true;
}
ResourceId Backend::createBuffer(std::span<const std::byte> data, bool index, bool dynamic) {
    auto& i=*impl; i.assertOwner(); if (data.empty() || data.size() > UINT32_MAX) throw std::runtime_error("Invalid buffer size");
    const auto id=i.resources.create(); auto& r=i.resources.get(id); r.bytes=unsigned(data.size());r.dynamic=dynamic; void* mapped=nullptr;
    try {
        const DWORD usage=D3DUSAGE_WRITEONLY|(dynamic?D3DUSAGE_DYNAMIC:0);const auto pool=dynamic?D3DPOOL_DEFAULT:D3DPOOL_MANAGED;
        if (index) { check(i.device->CreateIndexBuffer(r.bytes, usage, D3DFMT_INDEX16, pool, &r.ib, nullptr), "DX9 index buffer"); check(r.ib->Lock(0,0,&mapped,0), "DX9 index upload"); }
        else { check(i.device->CreateVertexBuffer(r.bytes,usage,0,pool,&r.vb,nullptr), "DX9 vertex buffer"); check(r.vb->Lock(0,0,&mapped,0), "DX9 vertex upload"); }
        std::memcpy(mapped,data.data(),data.size()); if (index) check(r.ib->Unlock(),"DX9 index unlock"); else check(r.vb->Unlock(),"DX9 vertex unlock");
    } catch (...) { i.resources.erase(id); throw; } return id;
}
void Backend::updateBuffer(ResourceId id,unsigned offset,std::span<const std::byte> data,bool discard,bool noOverwrite) {
    auto& i=*impl;i.assertOwner();auto& r=i.resources.get(id);if(offset>r.bytes||data.size()>r.bytes-offset) throw std::runtime_error("DX9 geometry update exceeds its bound");void* mapped=nullptr;
    if(discard&&noOverwrite)throw std::runtime_error("Invalid stock DISCARD/NOOVERWRITE combination");
    const auto flags=r.dynamic?(discard?D3DLOCK_DISCARD:noOverwrite?D3DLOCK_NOOVERWRITE:0):0;
    if(r.vb){check(r.vb->Lock(offset,unsigned(data.size()),&mapped,flags),"DX9 vertex update");std::memcpy(mapped,data.data(),data.size());check(r.vb->Unlock(),"DX9 vertex update unlock");}
    else if(r.ib){check(r.ib->Lock(offset,unsigned(data.size()),&mapped,flags),"DX9 index update");std::memcpy(mapped,data.data(),data.size());check(r.ib->Unlock(),"DX9 index update unlock");}else throw std::runtime_error("DX9 update is not geometry");
}
ResourceId Backend::createImage(const ImageDesc& d) {
    auto& i=*impl; i.assertOwner(); const auto id=i.resources.create(); auto& r=i.resources.get(id); r.desc=d;
    try {
        if (d.format==ImageFormat::DepthStencil) {
            if(d.sampled){check(i.device->CreateTexture(d.width,d.height,d.levels,D3DUSAGE_DEPTHSTENCIL,format(d.format),D3DPOOL_DEFAULT,&r.texture,nullptr),"DX9 sampled depth texture");check(r.texture->GetSurfaceLevel(0,&r.surface),"DX9 sampled depth surface");}
            else check(i.device->CreateDepthStencilSurface(d.width,d.height,format(d.format),d.samples==1?D3DMULTISAMPLE_NONE:D3DMULTISAMPLE_TYPE(d.samples),0,TRUE,&r.surface,nullptr), "DX9 depth image");
        }
        else if (d.samples!=1) check(i.device->CreateRenderTarget(d.width,d.height,format(d.format),D3DMULTISAMPLE_TYPE(d.samples),0,FALSE,&r.surface,nullptr),"DX9 MSAA target");
        else if (d.kind==ImageKind::Texture2D) { check(i.device->CreateTexture(d.width,d.height,d.levels,d.renderTarget?D3DUSAGE_RENDERTARGET:0,format(d.format),d.renderTarget?D3DPOOL_DEFAULT:D3DPOOL_MANAGED,&r.texture,nullptr),"DX9 texture"); check(r.texture->GetSurfaceLevel(0,&r.surface),"DX9 texture surface"); }
        else if (d.kind==ImageKind::Cube) check(i.device->CreateCubeTexture(d.width,d.levels,0,format(d.format),D3DPOOL_MANAGED,&r.cube,nullptr),"DX9 cube");
        else check(i.device->CreateVolumeTexture(d.width,d.height,d.depth,d.levels,0,format(d.format),D3DPOOL_MANAGED,&r.volume,nullptr),"DX9 volume");
    } catch (...) { i.resources.erase(id); throw; } return id;
}
void Backend::uploadImage(ResourceId id,unsigned level,unsigned layer,std::span<const std::byte> data) {
    auto& i=*impl; i.assertOwner(); auto& r=i.resources.get(id);
    if(level>=r.desc.levels || layer >= (r.desc.kind==ImageKind::Cube?6u:1u)) throw std::runtime_error("Invalid texture subresource");
    const unsigned w=(std::max)(1u,r.desc.width>>level),h=(std::max)(1u,r.desc.height>>level),z=(std::max)(1u,r.desc.depth>>level);
    const bool bc=r.desc.format==ImageFormat::Bc1||r.desc.format==ImageFormat::Bc2||r.desc.format==ImageFormat::Bc3;
    const auto rowBytes=bc?((w+3)/4)*(r.desc.format==ImageFormat::Bc1?8:16):w*(r.desc.format==ImageFormat::R8||r.desc.format==ImageFormat::Alpha8?1:r.desc.format==ImageFormat::Rg8?2:r.desc.format==ImageFormat::Rgba16Unorm||r.desc.format==ImageFormat::Rgba16Float?8:4); const unsigned rows=bc?(h+3)/4:h;
    if(data.size()!=std::size_t(rowBytes)*rows*z) throw std::runtime_error("Wrong texture data length");
    if(r.volume) { D3DLOCKED_BOX box{}; check(r.volume->LockBox(level,&box,nullptr,0),"DX9 volume upload"); for(unsigned zz=0;zz<z;++zz) for(unsigned y=0;y<rows;++y) std::memcpy(static_cast<std::byte*>(box.pBits)+zz*box.SlicePitch+y*box.RowPitch,data.data()+(zz*rows+y)*rowBytes,rowBytes); check(r.volume->UnlockBox(level),"DX9 volume unlock"); }
    else { D3DLOCKED_RECT rect{}; if(r.texture) check(r.texture->LockRect(level,&rect,nullptr,0),"DX9 texture upload"); else check(r.cube->LockRect(D3DCUBEMAP_FACES(layer),level,&rect,nullptr,0),"DX9 cube upload"); for(unsigned y=0;y<rows;++y) std::memcpy(static_cast<std::byte*>(rect.pBits)+y*rect.Pitch,data.data()+y*rowBytes,rowBytes); if(r.texture) check(r.texture->UnlockRect(level),"DX9 texture unlock"); else check(r.cube->UnlockRect(D3DCUBEMAP_FACES(layer),level),"DX9 cube unlock"); }
}
ResourceId Backend::createShader(std::span<const std::uint32_t> code,std::string) {
    auto& i=*impl; i.assertOwner(); if(code.empty()) throw std::runtime_error("Empty shader"); const auto id=i.resources.create(); auto& r=i.resources.get(id);
    try { if((code[0]>>16)==0xfffe) check(i.device->CreateVertexShader(reinterpret_cast<const DWORD*>(code.data()),&r.vs),"DX9 vertex shader"); else check(i.device->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()),&r.ps),"DX9 pixel shader"); } catch(...) { i.resources.erase(id); throw; } return id;
}
void Backend::release(ResourceId id) { impl->assertOwner(); if(!impl->resources.erase(id)) throw std::runtime_error("Stale DX9 resource"); }
void Backend::warm(const DrawPacket&) {impl->assertOwner();}
void Backend::resolve(ResourceId source,ResourceId destination) {auto& i=*impl;i.assertOwner();check(i.device->StretchRect(i.resources.get(source).surface,nullptr,i.resources.get(destination).surface,nullptr,D3DTEXF_NONE),"DX9 MSAA resolve");}
void Backend::beginFrame() { auto& i=*impl; i.assertOwner(); if(i.frame) throw std::runtime_error("Nested DX9 frame"); check(i.device->BeginScene(),"DX9 begin frame"); i.frame=true; ++i.timing.serial; i.timing.draws=0; i.start=std::chrono::steady_clock::now(); }
void Backend::clear(ResourceId color,ResourceId depth,const std::array<float,4>& rgba,float depthValue,
    unsigned stencilValue,bool clearDepth,bool clearStencil,std::span<const ClearRect> rectangles) {
    auto& i=*impl; i.assertOwner(); if(color)check(i.device->SetRenderTarget(0,i.resources.get(color).surface),"DX9 clear target");check(i.device->SetDepthStencilSurface(depth?i.resources.get(depth).surface:nullptr),"DX9 clear depth target"); auto channel=[](float v){return unsigned(std::clamp(v,0.f,1.f)*255+.5f);};
    if(rectangles.size()>256)throw std::runtime_error("Bounded stock clear rectangle count exceeded");
    std::array<D3DRECT,256> rects{};for(std::size_t n=0;n<rectangles.size();++n){const auto& r=rectangles[n];rects[n]={r.left,r.top,r.right,r.bottom};}
    const auto flags=(color?D3DCLEAR_TARGET:0)|(depth&&clearDepth?D3DCLEAR_ZBUFFER:0)|(depth&&clearStencil?D3DCLEAR_STENCIL:0);
    if(flags)check(i.device->Clear(unsigned(rectangles.size()),rectangles.empty()?nullptr:rects.data(),flags,D3DCOLOR_ARGB(channel(rgba[3]),channel(rgba[0]),channel(rgba[1]),channel(rgba[2])),depthValue,stencilValue),"DX9 clear");
}
void Backend::draw(const DrawPacket& d) {
    auto& i=*impl; i.assertOwner(); if(!i.frame) throw std::runtime_error("DX9 draw outside frame");
    if(d.index32) throw std::runtime_error("Comparison adapter requires index-format creation support");
    i.targets(d.colorTarget,d.depthTarget);
    D3DVIEWPORT9 viewport{DWORD(d.x),DWORD(d.y),DWORD(d.width),DWORD(d.height),d.minDepth,d.maxDepth}; check(i.device->SetViewport(&viewport),"DX9 viewport");
    check(i.device->SetVertexShader(i.resources.get(d.vertexShader).vs),"DX9 VS"); check(i.device->SetPixelShader(i.resources.get(d.pixelShader).ps),"DX9 PS");
    std::vector<D3DVERTEXELEMENT9> elements; for(const auto& e:d.elements) elements.push_back({WORD(e.stream),WORD(e.offset),BYTE(vertexMap[unsigned(e.format)]),D3DDECLMETHOD_DEFAULT,BYTE(e.semantic),BYTE(e.index)}); elements.push_back(D3DDECL_END());
    IDirect3DVertexDeclaration9* decl=nullptr; check(i.device->CreateVertexDeclaration(elements.data(),&decl),"DX9 vertex declaration"); const auto result=i.device->SetVertexDeclaration(decl); drop(decl); check(result,"DX9 declaration bind");
    auto state=[&](D3DRENDERSTATETYPE s,DWORD v){check(i.device->SetRenderState(s,v),"DX9 material state");};
    state(D3DRS_ZENABLE,d.state.depthTest); state(D3DRS_ZWRITEENABLE,d.state.depthWrite); state(D3DRS_ZFUNC,unsigned(d.state.depthCompare)+1); state(D3DRS_CULLMODE,d.state.cull?(d.state.clockwise?D3DCULL_CCW:D3DCULL_CW):D3DCULL_NONE);
    state(D3DRS_ALPHABLENDENABLE,d.state.blend); state(D3DRS_SRCBLEND,blendMap[unsigned(d.state.src)]); state(D3DRS_DESTBLEND,blendMap[unsigned(d.state.dst)]); state(D3DRS_BLENDOP,unsigned(d.state.blendOp)+1); state(D3DRS_COLORWRITEENABLE,d.state.colorMask);
    state(D3DRS_FILLMODE,d.state.wireframe?D3DFILL_WIREFRAME:D3DFILL_SOLID);
    state(D3DRS_DEPTHBIAS,std::bit_cast<DWORD>(d.state.depthBias));state(D3DRS_SLOPESCALEDEPTHBIAS,std::bit_cast<DWORD>(d.state.slopeDepthBias));
    state(D3DRS_SCISSORTESTENABLE,d.scissor);if(d.scissor){RECT box{d.scissorX,d.scissorY,LONG(d.scissorX+d.scissorWidth),LONG(d.scissorY+d.scissorHeight)};check(i.device->SetScissorRect(&box),"DX9 scissor");}
    state(D3DRS_SEPARATEALPHABLENDENABLE,TRUE); state(D3DRS_SRCBLENDALPHA,blendMap[unsigned(d.state.srcAlpha)]); state(D3DRS_DESTBLENDALPHA,blendMap[unsigned(d.state.dstAlpha)]); state(D3DRS_BLENDOPALPHA,unsigned(d.state.alphaBlendOp)+1); state(D3DRS_ALPHATESTENABLE,d.state.alphaTest); state(D3DRS_ALPHAFUNC,unsigned(d.state.alphaCompare)+1); state(D3DRS_ALPHAREF,d.state.alphaRef); state(D3DRS_STENCILENABLE,d.state.stencil); state(D3DRS_SRGBWRITEENABLE,i.resources.get(d.colorTarget).desc.srgb);
    state(D3DRS_STENCILFUNC,unsigned(d.state.front.compare)+1);state(D3DRS_STENCILFAIL,unsigned(d.state.front.fail)+1);state(D3DRS_STENCILZFAIL,unsigned(d.state.front.depthFail)+1);state(D3DRS_STENCILPASS,unsigned(d.state.front.pass)+1);
    state(D3DRS_TWOSIDEDSTENCILMODE,TRUE);state(D3DRS_CCW_STENCILFUNC,unsigned(d.state.back.compare)+1);state(D3DRS_CCW_STENCILFAIL,unsigned(d.state.back.fail)+1);state(D3DRS_CCW_STENCILZFAIL,unsigned(d.state.back.depthFail)+1);state(D3DRS_CCW_STENCILPASS,unsigned(d.state.back.pass)+1);state(D3DRS_STENCILREF,d.state.stencilRef);state(D3DRS_STENCILMASK,d.state.stencilReadMask);state(D3DRS_STENCILWRITEMASK,d.state.stencilWriteMask);
    if(!d.vertexConstants.empty()) check(i.device->SetVertexShaderConstantF(0,d.vertexConstants.data(),unsigned(d.vertexConstants.size()/4)),"DX9 vertex constants"); if(!d.pixelConstants.empty()) check(i.device->SetPixelShaderConstantF(0,d.pixelConstants.data(),unsigned(d.pixelConstants.size()/4)),"DX9 pixel constants");
    if(!d.vertexIntegers.empty()) check(i.device->SetVertexShaderConstantI(0,d.vertexIntegers.data(),unsigned(d.vertexIntegers.size()/4)),"DX9 vertex ints"); if(!d.pixelIntegers.empty()) check(i.device->SetPixelShaderConstantI(0,d.pixelIntegers.data(),unsigned(d.pixelIntegers.size()/4)),"DX9 pixel ints");
    if(!d.vertexBooleans.empty()) check(i.device->SetVertexShaderConstantB(0,d.vertexBooleans.data(),unsigned(d.vertexBooleans.size())),"DX9 vertex bools"); if(!d.pixelBooleans.empty()) check(i.device->SetPixelShaderConstantB(0,d.pixelBooleans.data(),unsigned(d.pixelBooleans.size())),"DX9 pixel bools");
    for(unsigned s=0;s<16;++s) if(d.textures[s]) {
        check(i.device->SetTexture(s,i.resources.get(d.textures[s]).base()),"DX9 texture bind"); const auto& a=d.samplers[s];
        auto sampler=[&](D3DSAMPLERSTATETYPE state,DWORD value){check(i.device->SetSamplerState(s,state,value),"DX9 sampler");};
        sampler(D3DSAMP_ADDRESSU,addressMap[unsigned(a.u)]); sampler(D3DSAMP_ADDRESSV,addressMap[unsigned(a.v)]); sampler(D3DSAMP_ADDRESSW,addressMap[unsigned(a.w)]); sampler(D3DSAMP_MAGFILTER,a.linear&&a.magLinear?D3DTEXF_LINEAR:D3DTEXF_POINT); sampler(D3DSAMP_MINFILTER,a.linear&&a.minLinear?D3DTEXF_LINEAR:D3DTEXF_POINT); sampler(D3DSAMP_MIPFILTER,a.mipLinear?D3DTEXF_LINEAR:D3DTEXF_POINT); sampler(D3DSAMP_SRGBTEXTURE,i.resources.get(d.textures[s]).desc.srgb);
    }
    unsigned vertices=0;
    for(unsigned s=0;s<16;++s) if(d.vertexBuffers[s]) { auto& r=i.resources.get(d.vertexBuffers[s]); check(i.device->SetStreamSource(s,r.vb,d.vertexOffsets[s],d.strides[s]),"DX9 stream"); if(!s) vertices=(r.bytes-d.vertexOffsets[s])/d.strides[s]; }
    check(i.device->SetIndices(i.resources.get(d.indexBuffer).ib),"DX9 index bind"); check(i.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,d.vertexBase,0,vertices,d.firstIndex,d.indexCount/3),"DX9 draw"); ++i.timing.draws;
}
FrameTiming Backend::endFrame(ResourceId source) {
    auto& i=*impl; i.assertOwner(); if(!i.frame) throw std::runtime_error("DX9 end outside frame"); check(i.device->EndScene(),"DX9 end frame"); i.frame=false; i.timing.recordMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-i.start).count();
    if(source) { IDirect3DSurface9* back=nullptr; check(i.device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back),"DX9 backbuffer"); const auto result=i.device->StretchRect(i.resources.get(source).surface,nullptr,back,nullptr,D3DTEXF_POINT); drop(back); check(result,"DX9 present copy"); check(i.device->Present(nullptr,nullptr,nullptr,nullptr),"DX9 present"); } return i.timing;
}
void Backend::blit(ResourceId source,ResourceId destination,bool linear,const ClearRect* sourceRect,const ClearRect* destinationRect) {
    auto& i=*impl;i.assertOwner();RECT s{},d{};
    if(sourceRect)s={sourceRect->left,sourceRect->top,sourceRect->right,sourceRect->bottom};
    if(destinationRect)d={destinationRect->left,destinationRect->top,destinationRect->right,destinationRect->bottom};
    check(i.device->StretchRect(i.resources.get(source).surface,sourceRect?&s:nullptr,i.resources.get(destination).surface,destinationRect?&d:nullptr,linear?D3DTEXF_LINEAR:D3DTEXF_POINT),"DX9 stock image blit");
}
std::vector<std::byte> Backend::readback(ResourceId id) {
    auto& i=*impl; i.assertOwner(); auto& r=i.resources.get(id); if(r.desc.format!=ImageFormat::Bgra8&&r.desc.format!=ImageFormat::Rgba8) throw std::runtime_error("Comparison readback requires RGBA8");
    IDirect3DSurface9* surface=nullptr; check(i.device->CreateOffscreenPlainSurface(r.desc.width,r.desc.height,format(r.desc.format),D3DPOOL_SYSTEMMEM,&surface,nullptr),"DX9 readback image");
    try { check(i.device->GetRenderTargetData(r.surface,surface),"DX9 lossless readback"); D3DLOCKED_RECT rect; check(surface->LockRect(&rect,nullptr,D3DLOCK_READONLY),"DX9 readback lock"); std::vector<std::byte> result(std::size_t(r.desc.width)*r.desc.height*4); for(unsigned y=0;y<r.desc.height;++y) std::memcpy(result.data()+y*r.desc.width*4,static_cast<std::byte*>(rect.pBits)+y*rect.Pitch,r.desc.width*4); check(surface->UnlockRect(),"DX9 readback unlock"); drop(surface); return result; } catch(...) { drop(surface); throw; }
}
void Backend::waitIdle() {
    auto& i=*impl;i.assertOwner();if(i.frame) throw std::runtime_error("DX9 idle barrier inside a frame");
    IDirect3DQuery9* event=nullptr;check(i.device->CreateQuery(D3DQUERYTYPE_EVENT,&event),"DX9 completion query");
    try {check(event->Issue(D3DISSUE_END),"DX9 idle signal");const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);HRESULT result;
        while((result=event->GetData(nullptr,0,D3DGETDATA_FLUSH))==S_FALSE){if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("DX9 idle barrier timed out");SwitchToThread();}check(result,"DX9 idle completion");drop(event);
    }catch(...){drop(event);throw;}
}
std::string Backend::deviceReport() const { return std::string("{\"api\":\"d3d9\",\"device\":\"")+impl->identifier.Description+"\"}"; }
}
