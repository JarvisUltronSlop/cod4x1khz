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
#include <atomic>
#include <vector>
#include <functional>

namespace {
IDirect3DQuery9* pollingEvent=nullptr;
unsigned contendedPolls=0;
std::function<void()> recordingUploadHook;
void recordingWithFencePoll(void (*task)(void*,unsigned),void* data,unsigned count){
    assert(pollingEvent);
    std::atomic<bool> returned=false;
    std::thread poller([&]{DWORD value=0;assert(pollingEvent->GetData(&value,4,D3DGETDATA_FLUSH)==S_FALSE);returned.store(true);});
    const auto deadline=GetTickCount64()+1000;
    while(!returned.load()){assert(GetTickCount64()<deadline);SwitchToThread();}
    poller.join();++contendedPolls;
    if(recordingUploadHook)recordingUploadHook();
    for(unsigned worker=0;worker<count;++worker)task(data,worker);
}
void check(HRESULT value){if(FAILED(value)){std::cerr<<gfx::legacy::lastError()<<std::endl;throw std::runtime_error("Legacy GPU HRESULT "+std::to_string(unsigned(value))+" : "+gfx::legacy::lastError());}}
template<class T> void release(T*& value){if(value)value->Release();value=nullptr;}
std::vector<DWORD> compile(const char* source,const char* profile){ID3DBlob *code=nullptr,*error=nullptr;const auto result=D3DCompile(source,std::strlen(source),"legacy fixture",nullptr,nullptr,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);if(error){std::cerr<<static_cast<const char*>(error->GetBufferPointer());error->Release();}check(result);std::vector<DWORD> out(code->GetBufferSize()/4);std::memcpy(out.data(),code->GetBufferPointer(),code->GetBufferSize());code->Release();return out;}
struct Window {HWND handle;Window(){handle=CreateWindowW(L"STATIC",L"Legacy graphics fixture",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!handle)throw std::runtime_error("Fixture window unavailable");}~Window(){DestroyWindow(handle);}};
struct Fixture {
    bool native=false;
    IDirect3D9* factory=nullptr;IDirect3DDevice9* device=nullptr;
    IDirect3DVertexBuffer9* vertices=nullptr;IDirect3DIndexBuffer9* indices=nullptr;
    IDirect3DVertexDeclaration9* declaration=nullptr;IDirect3DVertexShader9* vertex=nullptr;IDirect3DPixelShader9* pixel=nullptr;
    IDirect3DTexture9 *image=nullptr,*target=nullptr;IDirect3DSurface9 *surface=nullptr,*readback=nullptr;IDirect3DQuery9* event=nullptr;
    explicit Fixture(HWND window,bool vulkan,bool timings=false,bool contention=false,bool overlap=false,bool async=false,bool compare=true,unsigned flight=2):native(vulkan){
        if(vulkan){gfx::Config config;config.validation=true;config.compareD3D9=compare;config.framesInFlight=flight;config.recordFrameTimings=timings;config.overlapFrontendUploads=overlap;config.asyncSubmission=async;if(contention){config.recordingWorkers=2;config.dispatchRecording=recordingWithFencePoll;}factory=gfx::legacy::createVulkanInterface(config);}else factory=Direct3DCreate9(D3D_SDK_VERSION);
        if(!factory)throw std::runtime_error("Legacy factory unavailable");
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);if(GlobalMemoryStatusEx(&memory))
            std::cout<<"Fixture address-space free MiB: "<<memory.ullAvailVirtual/(1024*1024)<<std::endl;
        D3DPRESENT_PARAMETERS p{};p.BackBufferWidth=p.BackBufferHeight=64;p.BackBufferFormat=D3DFMT_A8R8G8B8;p.BackBufferCount=1;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=window;p.Windowed=TRUE;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        check(factory->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,&device));
        check(device->CreateQuery(D3DQUERYTYPE_EVENT,&event));if(contention)pollingEvent=event;DWORD data=0x5a5a5a5a;
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
        if(transfer==-5){
            // Read sparse never-written F/I/B registers, then expand those
            // prefixes after the first immutable draw has been recorded.
            const auto vs=compile("float4 padding:register(c20);struct I{float4 p:POSITION;float2 t:TEXCOORD0;};struct O{float4 p:POSITION;float2 t:TEXCOORD0;};O main(I i){O o;o.p=i.p+padding;o.t=i.t;return o;}","vs_3_0");
            // Authored PS3: r0=c0+c12; if(b6) rep(i9.x) r0+=c13.
            // HLSL globals use float registers even for int/bool types.
            const DWORD ps[]={0xffff0300,0x02000001,0x800f0000,0xa0e40000,
                0x03000002,0x800f0000,0x80e40000,0xa0e4000c,
                0x01000028,0xe0e40806,0x01000026,0xf0e40009,
                0x03000002,0x800f0000,0x80e40000,0xa0e4000d,
                0x00000027,0x0000002b,0x02000001,0x800f0800,0x80e40000,0xffff};
            release(vertex);release(pixel);check(device->CreateVertexShader(vs.data(),&vertex));check(device->CreatePixelShader(ps,&pixel));
        }
        check(device->SetRenderTarget(0,surface));check(device->SetDepthStencilSurface(nullptr));D3DVIEWPORT9 viewport{0,0,64,64,0,1};check(device->SetViewport(&viewport));
        check(device->SetRenderState(D3DRS_ZENABLE,FALSE));check(device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE));check(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(device->SetRenderState(D3DRS_LIGHTING,FALSE));
        check(device->SetVertexDeclaration(declaration));check(device->SetVertexShader(vertex));check(device->SetTexture(0,image));if(transfer>=0)textureTransfer(unsigned(transfer));check(device->SetPixelShader(pixel));check(device->SetStreamSource(0,vertices,0,24));check(device->SetIndices(indices));
        check(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
        check(device->BeginScene());check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0));
        const float brightness=transfer==-5?.25f:1.0f;const float tint[]={brightness,brightness,brightness,1},changed[]={0,0,0,0};check(device->SetPixelShaderConstantF(0,tint,1));check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
        if(transfer==-5){
            const BOOL disabled=FALSE,enabled=TRUE;const int zero[]={0,0,0,0},extra[]={1,0,0,0};
            const float step[]={.125f,0,0,0};check(device->SetPixelShaderConstantF(13,step,1));
            // Explicit zero writes extend the prefix too. Gaps remain zero.
            check(device->SetVertexShaderConstantF(20,changed,1));check(device->SetPixelShaderConstantF(12,changed,1));
            check(device->SetPixelShaderConstantI(9,zero,1));check(device->SetPixelShaderConstantB(6,&disabled,1));
            RECT box{0,0,32,32};check(device->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE));check(device->SetScissorRect(&box));
            check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            check(device->SetPixelShaderConstantI(9,extra,1));check(device->SetPixelShaderConstantB(6,&enabled,1));
            check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            // Mutations after recording must not change any earlier packet.
            const float moved[]={10,10,0,0};check(device->SetVertexShaderConstantF(20,moved,1));
            check(device->SetPixelShaderConstantI(9,zero,1));check(device->SetPixelShaderConstantB(6,&disabled,1));
        }
        if(transfer==-4)for(unsigned draw=1;draw<128;++draw)check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
        if(transfer==-6){
            // Repeated bindings share an immutable pipeline version; subsequent
            // material and shader changes must invalidate that version while
            // preserving all earlier captured draws and their resource leases.
            for(unsigned n=0;n<8;++n){check(device->SetTexture(0,image));check(device->SetStreamSource(0,vertices,0,24));check(device->SetVertexDeclaration(declaration));}
            RECT left{0,0,32,64},right{32,0,64,64},center{16,16,48,48};
            check(device->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE));check(device->SetScissorRect(&left));
            check(device->SetRenderState(D3DRS_COLORWRITEENABLE,4));const float halfRed[]={.5f,1,1,1};
            check(device->SetPixelShaderConstantF(0,halfRed,1));check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            check(device->SetScissorRect(&right));check(device->SetRenderState(D3DRS_COLORWRITEENABLE,1));const float halfBlue[]={1,1,.5f,1};
            check(device->SetPixelShaderConstantF(0,halfBlue,1));check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            const auto words=compile("sampler2D image:register(s0);float4 tint:register(c0);float4 main(float2 t:TEXCOORD0):COLOR0{return tex2D(image,t).bgra*tint;}","ps_3_0");
            IDirect3DPixelShader9* next=nullptr;check(device->CreatePixelShader(words.data(),&next));release(pixel);pixel=next;
            check(device->SetPixelShader(pixel));check(device->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(device->SetScissorRect(&center));
            check(device->SetPixelShaderConstantF(0,tint,1));check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            // A texture replacement and an address-mode change need different
            // descriptors despite otherwise identical pipeline/constant state.
            IDirect3DTexture9* patterned=nullptr;check(device->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&patterned,nullptr));
            D3DLOCKED_RECT texels{};check(patterned->LockRect(0,&texels,nullptr,0));
            for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(texels.pBits)+y*texels.Pitch)[x]=x<4?0xffc04020:0xff2060e0;
            check(patterned->UnlockRect(0));check(device->SetTexture(0,patterned));release(patterned);
            const auto tiled=compile("sampler2D image:register(s0);float4 tint:register(c0);float4 main(float2 t:TEXCOORD0):COLOR0{return tex2D(image,t*2)*tint;}","ps_3_0");
            next=nullptr;check(device->CreatePixelShader(tiled.data(),&next));release(pixel);pixel=next;check(device->SetPixelShader(pixel));
            check(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP));check(device->SetScissorRect(&left));
            check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            check(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(device->SetScissorRect(&right));
            check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
            check(device->SetTexture(0,image));check(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP));
        }
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
    void renderTargetResetsViewport(){
        D3DVIEWPORT9 partialViewport{3,5,20,24,.2f,.8f}, actual{};
        check(device->SetViewport(&partialViewport));
        assert(device->SetRenderTarget(0,nullptr)==D3DERR_INVALIDCALL);
        check(device->GetViewport(&actual));
        assert(actual.X==partialViewport.X&&actual.Y==partialViewport.Y&&actual.Width==partialViewport.Width&&actual.Height==partialViewport.Height);
        check(device->SetRenderTarget(0,surface));
        check(device->GetViewport(&actual));
        assert(actual.X==0&&actual.Y==0&&actual.Width==64&&actual.Height==64&&actual.MinZ==0&&actual.MaxZ==1);
        // Setting even the same surface resets viewport state again.
        check(device->SetViewport(&partialViewport));
        check(device->SetRenderTarget(0,surface));
        check(device->GetViewport(&actual));
        assert(actual.X==0&&actual.Y==0&&actual.Width==64&&actual.Height==64&&actual.MinZ==0&&actual.MaxZ==1);
    }
    void queuedFrameReuse(){
        const auto expected=run(); // establish all stock bindings and reference pixels
        const float geometry[][6]={{-.8f,.8f,.5f,1,0,0},{.8f,.8f,.5f,1,1,0},{.8f,-.8f,.5f,1,1,1},{-.8f,-.8f,.5f,1,0,1}};
        for(unsigned frame=0;frame<24;++frame){
            void* mapped=nullptr;check(vertices->Lock(0,sizeof(geometry),&mapped,D3DLOCK_DISCARD));
            std::memcpy(mapped,geometry,sizeof(geometry));check(vertices->Unlock());
            check(device->BeginScene());check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0));
            const float tint[]={frame==23?1.f:.5f,1,1,1};check(device->SetPixelShaderConstantF(0,tint,1));
            check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));check(device->EndScene());
            // Do not poll an event or wait idle between presentations. The
            // backend alone must protect upload/descriptor/frame-slot reuse.
            check(device->Present(nullptr,nullptr,nullptr,nullptr));
        }
        check(device->GetRenderTargetData(surface,readback));D3DLOCKED_RECT lock{};
        check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY));
        for(unsigned y=0;y<64;++y)assert(!std::memcmp(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch,expected.data()+y*64*4,64*4));
        check(readback->UnlockRect());assert(gfx::legacy::validationErrors(device)==0);
    }
    void completedQueryKeepsLaterPacketsQueued(){
        auto submissions=[](){const auto* field=std::strstr(gfx::legacy::frameTelemetry(), "\"submissions\":");assert(field);return std::stoull(field+14);};
        const auto before=submissions();
        check(device->BeginScene());check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff405060,1,0));
        DWORD value=0;assert(event->GetData(&value,4,D3DGETDATA_FLUSH)==S_OK);assert(value==1);
        assert(submissions()==before);
        check(device->EndScene());check(event->Issue(D3DISSUE_END));
        assert(event->GetData(&value,4,0)==S_FALSE);
        const auto deadline=GetTickCount64()+5000;
        while(event->GetData(&value,4,D3DGETDATA_FLUSH)==S_FALSE){assert(GetTickCount64()<deadline);SwitchToThread();}
        assert(submissions()==before+1);
    }
    void concurrentFrontendUpload(){
        const auto expected=run();
        std::thread producer;std::atomic<bool> started=false,returned=false;
        bool completedDuringRecording=false;std::exception_ptr failure;
        recordingUploadHook=[&]{
            if(started.exchange(true))return;
            producer=std::thread([&]{try{
                const float smaller[][6]={{-.2f,.2f,.5f,1,0,0},{.2f,.2f,.5f,1,1,0},{.2f,-.2f,.5f,1,1,1},{-.2f,-.2f,.5f,1,0,1}};
                void* mapped=nullptr;check(vertices->Lock(0,sizeof(smaller),&mapped,D3DLOCK_DISCARD));
                std::memcpy(mapped,smaller,sizeof(smaller));check(vertices->Unlock());
            }catch(...){failure=std::current_exception();}returned.store(true);});
            const auto deadline=GetTickCount64()+500;
            while(!returned.load()&&GetTickCount64()<deadline)SwitchToThread();
            completedDuringRecording=returned.load();
        };
        std::vector<BYTE> current;
        try{current=run(-4);}catch(...){recordingUploadHook={};if(producer.joinable())producer.join();throw;}
        recordingUploadHook={};assert(started);producer.join();if(failure)std::rethrow_exception(failure);
        assert(completedDuringRecording); // submission mutex is held by the owner here
        assert(current==expected); // future DISCARD must not change this frame
        const auto next=run();assert(next!=expected);
        const auto corner=(48*64+48)*4,center=(32*64+32)*4;
        assert(next[corner]==0x30&&next[corner+1]==0x20&&next[corner+2]==0x10&&next[corner+3]==255);
        assert(!std::memcmp(next.data()+center,expected.data()+center,4));
        assert(gfx::legacy::validationErrors(device)==0);
    }
    void sceneUploadAfterOwnerChange(){
        const auto expected=run();
        // A new dynamic buffer is deliberately uninitialized until the scene
        // thread writes it. Asset/loading ownership precedes that thread.
        release(vertices);check(device->CreateVertexBuffer(4*6*sizeof(float),D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&vertices,nullptr));
        check(device->SetStreamSource(0,vertices,0,24));
        std::vector<BYTE> actual(64*64*4);std::exception_ptr failure;
        std::thread renderer([&]{try{
            check(device->BeginScene());
            const float geometry[][6]={{-.8f,.8f,.5f,1,0,0},{.8f,.8f,.5f,1,1,0},{.8f,-.8f,.5f,1,1,1},{-.8f,-.8f,.5f,1,0,1}};
            void* mapped=nullptr;check(vertices->Lock(0,sizeof(geometry),&mapped,D3DLOCK_DISCARD));
            std::memcpy(mapped,geometry,sizeof(geometry));check(vertices->Unlock());
            const float tint[]={1,1,1,1};check(device->SetPixelShaderConstantF(0,tint,1));
            check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0));
            check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));check(device->EndScene());
            check(device->GetRenderTargetData(surface,readback));D3DLOCKED_RECT lock{};
            check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY));
            for(unsigned y=0;y<64;++y)std::memcpy(actual.data()+y*64*4,static_cast<BYTE*>(lock.pBits)+y*lock.Pitch,64*4);
            check(readback->UnlockRect());
        }catch(...){failure=std::current_exception();}});
        renderer.join();if(failure)std::rethrow_exception(failure);
        assert(actual==expected);assert(gfx::legacy::validationErrors(device)==0);
    }
    void asynchronousFrameConstruction(){
        const auto expected=run();
        std::atomic<bool> entered=false,released=false,timedOut=false;
        recordingUploadHook=[&]{entered.store(true);const auto deadline=GetTickCount64()+3000;
            while(!released.load()&&GetTickCount64()<deadline)SwitchToThread();
            if(!released.load())timedOut.store(true);};
        const float tint[]={1,1,1,1};
        check(device->BeginScene());check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0));check(device->SetPixelShaderConstantF(0,tint,1));
        for(unsigned n=0;n<128;++n)check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
        check(device->EndScene());check(event->Issue(D3DISSUE_END));check(device->Present(nullptr,nullptr,nullptr,nullptr));
        const auto deadline=GetTickCount64()+3000;while(!entered.load()&&GetTickCount64()<deadline)SwitchToThread();
        assert(entered.load());DWORD value=0;assert(event->GetData(&value,4,D3DGETDATA_FLUSH)==S_FALSE);
        // The first submission is deliberately held inside preparation. These
        // CPU calls must finish while its owner is still paused.
        const auto start=GetTickCount64();
        check(device->BeginScene());check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0));
        check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));check(device->EndScene());
        const auto cpuTime=GetTickCount64()-start;released.store(true);
        gfx::legacy::waitSubmissions(device);recordingUploadHook={};assert(!timedOut.load()&&cpuTime<500);
        check(device->Present(nullptr,nullptr,nullptr,nullptr));check(device->GetRenderTargetData(surface,readback));
        D3DLOCKED_RECT lock{};check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY));
        for(unsigned y=0;y<64;++y)assert(!std::memcmp(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch,expected.data()+y*64*4,64*4));
        check(readback->UnlockRect());assert(gfx::legacy::validationErrors(device)==0);
    }
    void asynchronousFailure(){
        run();const float tint[]={1,1,1,1};
        check(device->BeginScene());check(device->SetPixelShaderConstantF(0,tint,1));
        check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,1000));check(device->EndScene());
        check(device->Present(nullptr,nullptr,nullptr,nullptr));gfx::legacy::waitSubmissions(device);
        // The background error must reach the API, and destruction must join
        // without waiting for a timeline value that was never submitted.
        assert(FAILED(device->GetRenderTargetData(surface,readback)));
        assert(FAILED(device->Present(nullptr,nullptr,nullptr,nullptr)));
    }
    void completedQueriesDuringPreparation(){
        run();IDirect3DQuery9 *occlusion=nullptr,*timestamp=nullptr,*cachedEvent=nullptr;
        check(device->CreateQuery(D3DQUERYTYPE_OCCLUSION,&occlusion));
        check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&timestamp));
        check(device->CreateQuery(D3DQUERYTYPE_EVENT,&cachedEvent));
        const float tint[]={1,1,1,1};
        check(device->BeginScene());check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff102030,1,0));
        check(device->SetPixelShaderConstantF(0,tint,1));check(occlusion->Issue(D3DISSUE_BEGIN));
        check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));check(occlusion->Issue(D3DISSUE_END));
        check(timestamp->Issue(D3DISSUE_END));check(cachedEvent->Issue(D3DISSUE_END));
        check(device->EndScene());check(event->Issue(D3DISSUE_END));check(device->Present(nullptr,nullptr,nullptr,nullptr));
        gfx::legacy::waitSubmissions(device);
        DWORD expectedCount=0,eventValue=0;std::uint64_t expectedTimestamp=0;
        const auto deadline=GetTickCount64()+5000;
        while(cachedEvent->GetData(&eventValue,4,0)==S_FALSE){assert(GetTickCount64()<deadline);SwitchToThread();}
        check(occlusion->GetData(&expectedCount,4,0));assert(expectedCount>0&&expectedCount<=64*64);
        check(timestamp->GetData(&expectedTimestamp,8,0));assert(expectedTimestamp>0&&eventValue==1);
        std::atomic<bool> entered=false,released=false,timedOut=false;
        recordingUploadHook=[&]{entered.store(true);const auto until=GetTickCount64()+3000;
            while(!released.load()&&GetTickCount64()<until)SwitchToThread();if(!released.load())timedOut.store(true);};
        check(device->BeginScene());for(unsigned n=0;n<128;++n)check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2));
        check(device->EndScene());check(event->Issue(D3DISSUE_END));check(device->Present(nullptr,nullptr,nullptr,nullptr));
        const auto until=GetTickCount64()+3000;while(!entered.load()&&GetTickCount64()<until)SwitchToThread();assert(entered.load());
        DWORD actualCount=0;std::uint64_t actualTimestamp=0;
        assert(occlusion->GetData(&actualCount,4,0)==S_OK&&actualCount==expectedCount);
        assert(timestamp->GetData(&actualTimestamp,8,0)==S_OK&&actualTimestamp==expectedTimestamp);
        assert(cachedEvent->GetData(&eventValue,4,0)==S_OK&&eventValue==1);
        // A replacement issue must invalidate the cached result immediately,
        // even while another bank is held inside command preparation.
        check(cachedEvent->Issue(D3DISSUE_END));assert(cachedEvent->GetData(&eventValue,4,0)==S_FALSE);
        released.store(true);gfx::legacy::waitSubmissions(device);recordingUploadHook={};assert(!timedOut.load());
        check(device->Present(nullptr,nullptr,nullptr,nullptr));gfx::legacy::waitSubmissions(device);
        release(occlusion);release(timestamp);release(cachedEvent);assert(gfx::legacy::validationErrors(device)==0);
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
    const char* timings=nullptr;bool asyncOnly=false;
    for(int n=1;n<argc;++n){if(!std::strcmp(argv[n],"--timings")&&n+1<argc)timings=argv[++n];else if(!std::strcmp(argv[n],"--async-only"))asyncOnly=true;else return 2;}
    Window window;
    auto asyncCases=[&]{
        {Fixture f(window.handle,true,false,true,true,true);f.asynchronousFrameConstruction();f.concurrentFrontendUpload();f.completedQueriesDuringPreparation();}
        {Fixture f(window.handle,true,false,false,true,true,true,3);f.sceneUploadAfterOwnerChange();f.queuedFrameReuse();}
        {Fixture f(window.handle,true,false,false,true,true,false);f.asynchronousFailure();}
    };
    if(asyncOnly){asyncCases();std::cout<<"Bounded asynchronous frame construction, upload ordering, queries, failure and joined shutdown passed\n";return 0;}
    { Fixture native(window.handle,true), foreign(window.handle,false);
      foreign.renderTargetResetsViewport();native.renderTargetResetsViewport();
      gfx::legacy::nameResource(foreign.vertex,"foreign-dx9-vertex");
      gfx::legacy::nameResource(foreign.pixel,"foreign-dx9-pixel");
      assert(gfx::legacy::validationErrors(foreign.device)==UINT32_MAX);
      assert(!gfx::legacy::writeFrameTimings(foreign.device,"foreign-dx9-unused.csv"));
    }
    for(int transfer:{-6,-5,-4,-3,-2,-1,0,1,2}){std::vector<BYTE> reference,candidate;{Fixture f(window.handle,false);if(transfer==-3)f.frontendTransfers();reference=f.run(transfer);if(transfer==-5){for(unsigned channel=0;channel<3;++channel)assert(reference[(48*64+48)*4+channel]==64);assert(reference[(16*64+16)*4+2]==96);}}{Fixture f(window.handle,true,false,transfer==-4);if(transfer==-3)f.frontendTransfers();candidate=f.run(transfer);f.completedQueryKeepsLaterPacketsQueued();assert(gfx::legacy::validationErrors(f.device)==0);}assert(reference==candidate);}
    assert(contendedPolls>0);pollingEvent=nullptr;
    {Fixture f(window.handle,true);f.queuedFrameReuse();}
    {Fixture f(window.handle,true,false,true,true);f.concurrentFrontendUpload();}
    {Fixture f(window.handle,true,false,false,true);f.frontendTransfers();f.run();f.sceneUploadAfterOwnerChange();f.queuedFrameReuse();}
    asyncCases();
    if(timings){Fixture f(window.handle,true,true);for(unsigned n=0;n<24;++n){f.run();check(f.device->Present(nullptr,nullptr,nullptr,nullptr));}assert(gfx::legacy::writeFrameTimings(f.device,timings));assert(gfx::legacy::validationErrors(f.device)==0);}
    std::cout<<"Legacy resource/sparse immutable F/I/B constant/query, frontend upload and 2D/cube/volume transfer parity passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
