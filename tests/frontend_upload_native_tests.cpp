#include <universal/q_shared.h>
#include <khz/khz_frontend_upload.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx/legacy_interfaces.h>
#include <qcommon/threads.h>
#include <array>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#v); std::abort(); } } while (0)
GfxBackEndData s_backEndData[2];
GfxBackEndData* frontEndDataOut;
GfxBuffers gfxBuf;
DWORD threadId[THREAD_CONTEXT_COUNT];
static dvar_t staging{};
bool Sys_IsMainThread() { return GetCurrentThreadId()==threadId[THREAD_CONTEXT_MAIN]; }
bool Sys_IsRenderThread() { return GetCurrentThreadId()==threadId[THREAD_CONTEXT_BACKEND]; }
uint32_t Sys_GetCurrentThreadId() { return GetCurrentThreadId(); }
ThreadContext_t Sys_WorkerContext(unsigned index) {
    return index<2?static_cast<ThreadContext_t>(index+2):index==2?THREAD_CONTEXT_WORKER2:THREAD_CONTEXT_WORKER3;
}
const dvar_t* Dvar_RegisterBool(const char*, bool value, unsigned short, const char*) { staging.current.enabled=value;return &staging; }
char* Z_VirtualAlloc(int bytes,const char*,int) {
    auto* memory=static_cast<char*>(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    CHECK(memory);std::memset(memory,0xcd,bytes);return memory;
}
void Z_VirtualFree(void* memory) { CHECK(VirtualFree(memory,0,MEM_RELEASE)); }
void R_FatalLockError(HRESULT) { std::abort(); }
void Com_Error(errorParm_t,const char*,...) { std::abort(); }
void MyAssertHandler(const char*,int,int,const char*,...) { std::abort(); }
namespace gfx::legacy { HRESULT unsupported(const char*) { std::abort(); } }
namespace {
class VertexBuffer final : public gfx::legacy::IDirect3DVertexBuffer9Base {
public:
    std::array<unsigned char,6*sizeof(GfxWorldVertex)> bytes{};
    unsigned locks=0;
    HRESULT STDMETHODCALLTYPE Lock(UINT offset,UINT count,void** out,DWORD) override {
        CHECK(offset<=bytes.size()&&count<=bytes.size()-offset);*out=bytes.data()+offset;++locks;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { return S_OK; }
};
VertexBuffer vertices[2][3];
void write(VertexBuffer& target,const void* values,unsigned offset,unsigned count,unsigned flags) {
    void* memory=nullptr;
    if(!KHZ_FrontendVertexLock(&target,0,target.bytes.size(),flags,&memory))
        CHECK(SUCCEEDED(target.Lock(0,target.bytes.size(),&memory,flags)));
    std::memcpy(static_cast<unsigned char*>(memory)+offset,values,count);
    if(!KHZ_FrontendVertexUnlock(&target))CHECK(SUCCEEDED(target.Unlock()));
}
}
int main() {
    threadId[THREAD_CONTEXT_MAIN]=GetCurrentThreadId();
    for(unsigned slot=0;slot<2;++slot) {
        auto& data=s_backEndData[slot];
        data.skinnedCacheVb=&gfxBuf.skinnedCacheVbPool[slot];
        data.skinnedCacheVb->total=sizeof(vertices[slot][0].bytes);data.skinnedCacheVb->buffer=&vertices[slot][0];
        data.codeMesh.vb.total=sizeof(vertices[slot][1].bytes);data.codeMesh.vb.buffer=&vertices[slot][1];
        data.markMesh.vb.total=sizeof(vertices[slot][2].bytes);data.markMesh.vb.buffer=&vertices[slot][2];
        gfxBuf.preTessIndexBufferPool[slot].total=32;
    }
    KHZ_FrontendUploadInit();
    for(unsigned frame=1;frame<=8;++frame) {
        const unsigned slot=frame%2;auto& data=s_backEndData[slot];frontEndDataOut=&data;data.frameCount=frame;
        GfxWorldVertex expected[6]{};
        for(unsigned n=0;n<6;++n) {expected[n].xyz[0]=frame*100.f+n;expected[n].xyz[1]=n*2.f;expected[n].xyz[2]=60.f;}
        std::barrier start(2);
        const unsigned markWorker=(frame-1)%4,fxWorker=(markWorker+1)%4;
        std::thread marks([&]{threadId[Sys_WorkerContext(markWorker)]=GetCurrentThreadId();start.arrive_and_wait();
            // WRKCMD_GENERATE_MARK_VERTS writes the world prefix on a worker.
            write(vertices[slot][2],expected,0,3*sizeof(GfxWorldVertex),D3DLOCK_DISCARD);});
        std::thread effects([&]{threadId[Sys_WorkerContext(fxWorker)]=GetCurrentThreadId();start.arrive_and_wait();
            write(vertices[slot][1],expected,0,sizeof(expected),D3DLOCK_DISCARD);});
        marks.join();effects.join();
        threadId[Sys_WorkerContext(markWorker)]=threadId[Sys_WorkerContext(fxWorker)]=0;
        // R_WaitWorkerCmdsOfType completes before static/dynamic model marks
        // append on the main thread using a full-range NOOVERWRITE lock.
        write(vertices[slot][2],expected+3,3*sizeof(GfxWorldVertex),3*sizeof(GfxWorldVertex),D3DLOCK_NOOVERWRITE);
        data.markMesh.vb.used=sizeof(expected);data.codeMesh.vb.used=sizeof(expected);
        KHZ_FrontendUpload(&data);
        if(std::memcmp(vertices[slot][2].bytes.data(),expected,sizeof(expected))) {
            std::fputs("World decal vertices were overwritten when model marks appended after a worker lock\n",stderr);return 1;
        }
        CHECK(!std::memcmp(vertices[slot][1].bytes.data(),expected,sizeof(expected)));
        CHECK(vertices[slot][2].locks==(frame+1)/2); // one GPU upload per completed frame
        CHECK(vertices[slot][1].locks==(frame+1)/2);
    }
    // Extending access to frontend workers must not admit arbitrary threads
    // or let the render owner start preparing the next frontend frame.
    std::thread unrelated([&]{
        void* memory=nullptr;
        CHECK(!KHZ_FrontendVertexLock(&vertices[0][2],0,4,D3DLOCK_DISCARD,&memory));
        threadId[THREAD_CONTEXT_BACKEND]=GetCurrentThreadId();
        CHECK(!KHZ_FrontendVertexLock(&vertices[0][2],0,4,D3DLOCK_DISCARD,&memory));
        CHECK(!KHZ_FrontendVertexUnlock(&vertices[0][2]));
    });
    unrelated.join();threadId[THREAD_CONTEXT_BACKEND]=0;
    staging.current.enabled=false;frontEndDataOut->frameCount=9;
    void* memory=nullptr;CHECK(!KHZ_FrontendVertexLock(&vertices[0][2],0,4,D3DLOCK_DISCARD,&memory));
    KHZ_FrontendUploadShutdown();
    std::puts("Native world-worker/model-main decal staging, all four workers, concurrent effects, slot reuse, owner exclusion and disabled fallback passed");
}
