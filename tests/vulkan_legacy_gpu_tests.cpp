#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "gfx/legacy_factory.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <exception>
#include <vector>

namespace {
void check(HRESULT value){if(FAILED(value))throw std::runtime_error("Legacy GPU HRESULT "+std::to_string(unsigned(value)));}
template<class T> void release(T*& value){if(value)value->Release();value=nullptr;}
std::vector<DWORD> compile(const char* source,const char* profile){ID3DBlob *code=nullptr,*error=nullptr;const auto result=D3DCompile(source,std::strlen(source),"legacy fixture",nullptr,nullptr,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);if(error){std::cerr<<static_cast<const char*>(error->GetBufferPointer());error->Release();}check(result);std::vector<DWORD> out(code->GetBufferSize()/4);std::memcpy(out.data(),code->GetBufferPointer(),code->GetBufferSize());code->Release();return out;}
struct Window {HWND handle;Window(){handle=CreateWindowW(L"STATIC",L"Legacy graphics fixture",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!handle)throw std::runtime_error("Fixture window unavailable");}~Window(){DestroyWindow(handle);}};
struct Fixture {
    bool native=false;
    IDirect3D9* factory=nullptr;IDirect3DDevice9* device=nullptr;
    IDirect3DVertexBuffer9* vertices=nullptr;IDirect3DIndexBuffer9* indices=nullptr;
    IDirect3DVertexDeclaration9* declaration=nullptr;IDirect3DVertexShader9* vertex=nullptr;IDirect3DPixelShader9* pixel=nullptr;
    IDirect3DTexture9 *image=nullptr,*target=nullptr;IDirect3DSurface9 *surface=nullptr,*readback=nullptr;IDirect3DQuery9* event=nullptr;
    explicit Fixture(HWND window,bool vulkan,bool timings=false):native(vulkan){
        if(vulkan){gfx::Config config;config.validation=true;config.compareD3D9=true;config.recordFrameTimings=timings;factory=gfx::legacy::createVulkanInterface(config);}else factory=Direct3DCreate9(D3D_SDK_VERSION);
        if(!factory)throw std::runtime_error("Legacy factory unavailable");
        D3DPRESENT_PARAMETERS p{};p.BackBufferWidth=p.BackBufferHeight=64;p.BackBufferFormat=D3DFMT_A8R8G8B8;p.BackBufferCount=1;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=window;p.Windowed=TRUE;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        check(factory->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,&device));
        check(device->CreateQuery(D3DQUERYTYPE_EVENT,&event));DWORD data=0x5a5a5a5a;
        const auto idle=event->GetData(&data,4,D3DGETDATA_FLUSH);
        std::cout<<(vulkan?"Vulkan":"D3D9")<<" unissued event: "<<std::hex<<unsigned(idle)<<std::dec<<" value="<<data<<std::endl;
        // Idle-event output bytes are undefined on the actual D3D9 driver;
        // the stock loading barrier depends on its ready HRESULT only.
        assert(idle==S_OK);
        const float geometry[][6]={{-.8f,.8f,.5f,1,0,0},{.8f,.8f,.5f,1,1,0},{.8f,-.8f,.5f,1,1,1},{-.8f,-.8f,.5f,1,0,1}};
        check(device->CreateVertexBuffer(sizeof(geometry),D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&vertices,nullptr));void* mapped=nullptr;check(vertices->Lock(0,sizeof(geometry),&mapped,D3DLOCK_DISCARD));std::memcpy(mapped,geometry,sizeof(geometry));check(vertices->Unlock());
        const WORD elements[]={0,1,2,0,2,3};check(device->CreateIndexBuffer(sizeof(elements),D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_MANAGED,&indices,nullptr));check(indices->Lock(0,0,&mapped,0));std::memcpy(mapped,elements,sizeof(elements));check(indices->Unlock());
        const D3DVERTEXELEMENT9 decl[]={{0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},{0,16,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};check(device->CreateVertexDeclaration(decl,&declaration));
        const auto vs=compile("struct I{float4 p:POSITION;float2 t:TEXCOORD0;};struct O{float4 p:POSITION;float2 t:TEXCOORD0;};O main(I i){O o;o.p=i.p;o.t=i.t;return o;}","vs_3_0");
        const auto ps=compile("sampler2D image:register(s0);float4 tint:register(c0);float4 main(float2 t:TEXCOORD0):COLOR0{return tex2D(image,t)*tint;}","ps_3_0");
        check(device->CreateVertexShader(vs.data(),&vertex));check(device->CreatePixelShader(ps.data(),&pixel));
        check(device->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&image,nullptr));D3DLOCKED_RECT lock{};check(image->LockRect(0,&lock,nullptr,0));for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch)[x]=0xffb04020;check(image->UnlockRect(0));
        check(device->CreateTexture(64,64,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&target,nullptr));check(target->GetSurfaceLevel(0,&surface));
        check(device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr));
    }
    void textureTransfer(unsigned kind){
        IDirect3DBaseTexture9 *source=nullptr,*destination=nullptr;
        if(kind==0){IDirect3DTexture9 *s=nullptr,*d=nullptr;check(device->CreateTexture(8,8,2,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&s,nullptr));check(device->CreateTexture(8,8,2,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&d,nullptr));source=s;destination=d;
            for(unsigned mip=0;mip<2;++mip){D3DLOCKED_RECT lock{};check(s->LockRect(mip,&lock,nullptr,D3DLOCK_NO_DIRTY_UPDATE));const auto size=8u>>mip;for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch)[x]=0xffa05030;check(s->UnlockRect(mip));}check(s->AddDirtyRect(nullptr));
        }else if(kind==1){IDirect3DCubeTexture9 *s=nullptr,*d=nullptr;check(device->CreateCubeTexture(8,2,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&s,nullptr));check(device->CreateCubeTexture(8,2,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&d,nullptr));source=s;destination=d;
            for(unsigned face=0;face<6;++face){for(unsigned mip=0;mip<2;++mip){D3DLOCKED_RECT lock{};check(s->LockRect(D3DCUBEMAP_FACES(face),mip,&lock,nullptr,D3DLOCK_NO_DIRTY_UPDATE));const auto size=8u>>mip;for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch)[x]=0xff805020+(face<<16);check(s->UnlockRect(D3DCUBEMAP_FACES(face),mip));}check(s->AddDirtyRect(D3DCUBEMAP_FACES(face),nullptr));}
        }else{IDirect3DVolumeTexture9 *s=nullptr,*d=nullptr;check(device->CreateVolumeTexture(8,8,8,2,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&s,nullptr));check(device->CreateVolumeTexture(8,8,8,2,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&d,nullptr));source=s;destination=d;
            for(unsigned mip=0;mip<2;++mip){D3DLOCKED_BOX lock{};check(s->LockBox(mip,&lock,nullptr,D3DLOCK_NO_DIRTY_UPDATE));const auto size=8u>>mip;for(unsigned z=0;z<size;++z)for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+z*lock.SlicePitch+y*lock.RowPitch)[x]=0xff906040;check(s->UnlockBox(mip));}check(s->AddDirtyBox(nullptr));}
        check(device->UpdateTexture(source,destination));check(device->SetTexture(0,destination));release(source);release(destination);
        if(kind){const auto words=compile(kind==1?"samplerCUBE image:register(s0);float4 tint:register(c0);float4 main(float2 t:TEXCOORD0):COLOR0{return texCUBE(image,float3(1,t.x-.5,t.y-.5))*tint;}":"sampler3D image:register(s0);float4 tint:register(c0);float4 main(float2 t:TEXCOORD0):COLOR0{return tex3D(image,float3(t,.5))*tint;}","ps_3_0");IDirect3DPixelShader9* next=nullptr;check(device->CreatePixelShader(words.data(),&next));release(pixel);pixel=next;}
    }
    std::vector<BYTE> run(int transfer=-1){
        check(device->SetRenderTarget(0,surface));check(device->SetDepthStencilSurface(nullptr));D3DVIEWPORT9 viewport{0,0,64,64,0,1};check(device->SetViewport(&viewport));
        check(device->SetRenderState(D3DRS_ZENABLE,FALSE));check(device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE));check(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(device->SetRenderState(D3DRS_LIGHTING,FALSE));
        check(device->SetVertexDeclaration(declaration));check(device->SetVertexShader(vertex));check(device->SetTexture(0,image));if(transfer>=0)textureTransfer(unsigned(transfer));check(device->SetPixelShader(pixel));check(device->SetStreamSource(0,vertices,0,24));check(device->SetIndices(indices));
        check(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
        check(device->BeginScene());check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0));
        const float tint[]={1,1,1,1},changed[]={0,0,0,0};check(device->SetPixelShaderConstantF(0,tint,1));check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
        if(transfer==-2){
            // Distinct immutable register versions and repeated unchanged
            // versions coexist in one queued submission.
            const float alternate[]={.5f,.25f,.75f,1};RECT box{0,0,32,32};
            check(device->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE));check(device->SetScissorRect(&box));
            check(device->SetPixelShaderConstantF(0,alternate,1));check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            check(device->SetPixelShaderConstantF(0,alternate,1));check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
        }
        // Mutating source constants after a draw must not affect its packet.
        check(device->SetPixelShaderConstantF(0,changed,1));check(device->EndScene());check(event->Issue(D3DISSUE_END));
        const auto deadline=GetTickCount64()+5000;DWORD value=0;HRESULT status;
        while((status=event->GetData(&value,4,D3DGETDATA_FLUSH))==S_FALSE){if(GetTickCount64()>=deadline)throw std::runtime_error("Legacy event timed out");SwitchToThread();}check(status);assert(value==1);
        if(transfer==-2){
            // The same CPU register values need a new arena snapshot after
            // retirement. Drop the final shader/texture owners before flush;
            // retained packet leases must keep the recorded resources alive.
            const float alternate[]={.5f,.25f,.75f,1};RECT box{32,32,64,64};
            check(device->BeginScene());check(device->SetScissorRect(&box));check(device->SetPixelShaderConstantF(0,alternate,1));
            check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            check(device->SetPixelShader(nullptr));release(pixel);check(device->SetTexture(0,nullptr));release(image);check(device->EndScene());
        }
        check(event->Issue(D3DISSUE_END));
        // Vulkan's command packet has not yet been submitted. Its previous
        // completed issue must not make this one appear complete.
        if(native)assert(event->GetData(&value,4,0)==S_FALSE);
        check(device->GetRenderTargetData(surface,readback));D3DLOCKED_RECT lock{};check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY));std::vector<BYTE> pixels(64*64*4);for(unsigned y=0;y<64;++y)std::memcpy(pixels.data()+y*64*4,static_cast<BYTE*>(lock.pBits)+y*lock.Pitch,64*4);check(readback->UnlockRect());return pixels;
    }
    void frontendTransfers(){
        // The renderer owns the GPU submission; a later frontend upload must
        // retain immutable bytes and its destination until the next flush.
        IDirect3DTexture9 *source=nullptr,*next=nullptr,*retired=nullptr;
        check(device->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&source,nullptr));
        check(device->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&next,nullptr));
        check(device->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&retired,nullptr));
        std::exception_ptr failure;
        std::thread renderer([&]{try{run();}catch(...){failure=std::current_exception();}});
        renderer.join();if(failure)std::rethrow_exception(failure);
        const float geometry[][6]={{-.8f,.8f,.5f,1,0,0},{.8f,.8f,.5f,1,1,0},{.8f,-.8f,.5f,1,1,1},{-.8f,-.8f,.5f,1,0,1}};
        void* mapped=nullptr;check(vertices->Lock(0,sizeof(geometry),&mapped,D3DLOCK_DISCARD));std::memcpy(mapped,geometry,sizeof(geometry));check(vertices->Unlock());
        D3DLOCKED_RECT lock{};check(source->LockRect(0,&lock,nullptr,0));
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch)[x]=0xff2080c0;
        check(source->UnlockRect(0));check(device->UpdateTexture(source,next));
        check(source->AddDirtyRect(nullptr));check(device->UpdateTexture(source,retired));release(retired);
        // The source can be rewritten without dirtying the destination. The
        // already queued transfer must still contain the earlier blue pixels.
        check(source->LockRect(0,&lock,nullptr,D3DLOCK_NO_DIRTY_UPDATE));
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch)[x]=0xff00ff00;
        check(source->UnlockRect(0));release(source);release(image);image=next;
    }
    ~Fixture(){release(event);release(surface);release(readback);release(target);release(image);release(declaration);release(vertex);release(pixel);release(vertices);release(indices);release(device);release(factory);}
};
}
int main(int argc,char** argv){try{
    const char* timings=nullptr;if(argc==3&&std::strcmp(argv[1],"--timings")==0)timings=argv[2];else if(argc!=1)return 2;
    Window window;for(int transfer:{-3,-2,-1,0,1,2}){std::vector<BYTE> reference,candidate;{Fixture f(window.handle,false);if(transfer==-3)f.frontendTransfers();reference=f.run(transfer);}{Fixture f(window.handle,true);if(transfer==-3)f.frontendTransfers();candidate=f.run(transfer);assert(gfx::legacy::validationErrors(f.device)==0);}assert(reference==candidate);}
    if(timings){Fixture f(window.handle,true,true);for(unsigned n=0;n<24;++n){f.run();check(f.device->Present(nullptr,nullptr,nullptr,nullptr));}assert(gfx::legacy::writeFrameTimings(f.device,timings));assert(gfx::legacy::validationErrors(f.device)==0);}
    std::cout<<"Legacy resource/constant/query, frontend upload and 2D/cube/volume transfer parity passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
