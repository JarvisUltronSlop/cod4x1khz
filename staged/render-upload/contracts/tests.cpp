#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

constexpr int D3DLOCK_DISCARD = 8192, D3DLOCK_NOOVERWRITE = 4096, DVAR_NOFLAG = 0, ERR_FATAL = 0;
static unsigned checks = 0, gpuCalls = 0, allocCalls = 0, rejected = 0;
static int role = 0;
static bool Sys_IsMainThread() { return role == 0; }
static bool Sys_IsRenderThread() { return role == 1; }
struct dvar_t { struct { bool enabled = true; } current; } mode;
static const dvar_t* Dvar_RegisterBool(const char*, bool, int, const char*) { return &mode; }
[[noreturn]] static void Com_Error(int, const char*, const char* message) { throw std::runtime_error(message); }
[[noreturn]] static void R_FatalLockError(HRESULT) { throw std::runtime_error("D3D boundary failed"); }
static std::unordered_map<void*, unsigned> allocations;
static char* Z_VirtualAlloc(unsigned bytes, const char*, int) {
    const unsigned size = (bytes + 4095) & ~4095u;
    void* p = VirtualAlloc(nullptr, size + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) throw std::runtime_error("allocation failed");
    DWORD old;
    if (!VirtualProtect(static_cast<char*>(p) + size, 4096, PAGE_NOACCESS, &old)) throw std::runtime_error("guard failed");
    allocations[p] = size; ++allocCalls;
    return static_cast<char*>(p);
}
static void Z_VirtualFree(void* p) { allocations.erase(p); VirtualFree(p, 0, MEM_RELEASE); }
struct MockBuffer {
    std::array<unsigned char, 4096> data;
    unsigned lastBytes = 0;
    bool locked = false;
    MockBuffer() { data.fill(0xcd); }
    HRESULT Lock(unsigned offset, unsigned bytes, void** out, int flags) {
        ++gpuCalls;
        if (role != 1 || locked || offset || bytes > data.size() || flags != D3DLOCK_DISCARD) return E_FAIL;
        locked = true; lastBytes = bytes; *out = data.data(); return S_OK;
    }
    HRESULT Unlock() { if (!locked) return E_FAIL; locked = false; return S_OK; }
};
struct IDirect3DVertexBuffer9 : MockBuffer {};
struct IDirect3DIndexBuffer9 : MockBuffer {};
struct Vertex { IDirect3DVertexBuffer9* buffer = nullptr; int used = 0, total = 4096; };
struct Index { IDirect3DIndexBuffer9* buffer = nullptr; int used = 0, total = 2048; };
struct Mesh { Vertex vb; };
struct GfxBackEndData { Vertex* skinnedCacheVb; Mesh codeMesh, markMesh; IDirect3DIndexBuffer9* preTessIb; unsigned frameCount = 1; };
static GfxBackEndData s_backEndData[2], *frontEndDataOut;
static struct { Vertex skinnedCacheVbPool[2]; Index preTessIndexBufferPool[2], *preTessIndexBuffer; } gfxBuf;
#include "generated/native_upload.inc"

static void check(bool yes) { ++checks; if (!yes) throw std::runtime_error("contract assertion failed"); }
template<class F> static void fails(F f) {
    try { f(); } catch (const std::runtime_error&) { ++rejected; ++checks; return; }
    throw std::runtime_error("negative control accepted");
}
static void select(unsigned slot, unsigned frame) {
    role = 0; frontEndDataOut = &s_backEndData[slot]; frontEndDataOut->frameCount = frame;
    gfxBuf.preTessIndexBuffer = &gfxBuf.preTessIndexBufferPool[slot];
}
int main() {
    try {
        IDirect3DVertexBuffer9 vertices[2][3]; IDirect3DIndexBuffer9 indices[2];
        for (unsigned s = 0; s != 2; ++s) {
            auto& data = s_backEndData[s];
            gfxBuf.skinnedCacheVbPool[s].buffer = &vertices[s][0];
            gfxBuf.preTessIndexBufferPool[s].buffer = &indices[s];
            data.skinnedCacheVb = &gfxBuf.skinnedCacheVbPool[s];
            data.codeMesh.vb.buffer = &vertices[s][1]; data.markMesh.vb.buffer = &vertices[s][2]; data.preTessIb = &indices[s];
        }
        KHZ_FrontendUploadInit(); check(allocations.size() == 8);
        const unsigned fixedAllocations = allocCalls;
        for (unsigned frame = 1; frame <= 1024; ++frame) {
            const unsigned s = frame & 1; select(s, frame);
            auto& data = *frontEndDataOut;
            void* ptr = nullptr;
            for (unsigned b = 0; b != 3; ++b) {
                check(KHZ_FrontendVertexLock(&vertices[s][b], 0, b ? 4096 : 0, D3DLOCK_DISCARD, &ptr));
                const unsigned bytes = (frame + b) % 4096 + 1;
                std::memset(ptr, (frame + b) & 255, bytes);
                if (!b) data.skinnedCacheVb->used = bytes;
                else if (b == 1) data.codeMesh.vb.used = bytes;
                else data.markMesh.vb.used = bytes;
                check(KHZ_FrontendVertexUnlock(&vertices[s][b]));
            }
            check(KHZ_FrontendIndexLock(&indices[s], 0, 4096, D3DLOCK_DISCARD, &ptr));
            gfxBuf.preTessIndexBuffer->used = frame % 2048 + 1;
            std::memset(ptr, frame & 255, 2 * gfxBuf.preTessIndexBuffer->used);
            check(KHZ_FrontendIndexUnlock(&indices[s]));
            const unsigned prior = gpuCalls;
            role = 1; KHZ_FrontendUpload(&data);
            check(gpuCalls == prior + 4);
            for (unsigned b = 0; b != 3; ++b) {
                const auto& v = vertices[s][b];
                check(v.lastBytes == (frame + b) % 4096 + 1);
                check(std::all_of(v.data.begin(), v.data.begin() + v.lastBytes, [=](unsigned char c) { return c == ((frame + b) & 255); }));
            }
            check(indices[s].lastBytes == 2 * (frame % 2048 + 1));
            check(std::all_of(indices[s].data.begin(), indices[s].data.begin() + indices[s].lastBytes, [=](unsigned char c) { return c == (frame & 255); }));
            KHZ_FrontendUpload(&data); check(gpuCalls == prior + 4); // No duplicate upload.
            check(allocCalls == fixedAllocations); // No steady-frame allocation.
        }
        void* p = nullptr; select(0, 2000);
        IDirect3DVertexBuffer9 unrelated;
        check(!KHZ_FrontendVertexLock(&unrelated, 0, 0, D3DLOCK_DISCARD, &p));
        fails([&] { KHZ_FrontendVertexLock(&vertices[0][0], 1, 1, D3DLOCK_DISCARD, &p); });
        fails([&] { KHZ_FrontendVertexLock(&vertices[0][0], 0, 4097, D3DLOCK_DISCARD, &p); });
        fails([&] { KHZ_FrontendVertexLock(&vertices[0][0], 0, -1, D3DLOCK_DISCARD, &p); });
        fails([&] { KHZ_FrontendVertexLock(&vertices[0][0], 0, 1, 0, &p); });
        check(KHZ_FrontendVertexLock(&vertices[0][0], 0, 0, D3DLOCK_DISCARD, &p));
        fails([&] { KHZ_FrontendVertexLock(&vertices[0][0], 0, 0, D3DLOCK_DISCARD, &p); });
        role = 1; fails([&] { KHZ_FrontendUpload(frontEndDataOut); });
        role = 0; check(KHZ_FrontendVertexUnlock(&vertices[0][0]));
        // Changing the dvar after a CPU lock must still publish its pending data.
        mode.current.enabled = false; role = 1; KHZ_FrontendUpload(frontEndDataOut);
        select(1, 2001); check(!KHZ_FrontendVertexLock(&vertices[1][0], 0, 0, D3DLOCK_DISCARD, &p));
        mode.current.enabled = true; check(!KHZ_FrontendVertexLock(&vertices[1][0], 0, 0, D3DLOCK_DISCARD, &p));
        select(1, 2002); check(KHZ_FrontendVertexLock(&vertices[1][0], 0, 0, D3DLOCK_DISCARD, &p));
        frontEndDataOut->skinnedCacheVb->used = 4097;
        check(KHZ_FrontendVertexUnlock(&vertices[1][0]));
        role = 1; fails([&] { KHZ_FrontendUpload(frontEndDataOut); });
        select(0, 2003); check(KHZ_FrontendIndexLock(&indices[0], 0, 4096, D3DLOCK_DISCARD, &p));
        gfxBuf.preTessIndexBuffer->used = -1;
        fails([&] { KHZ_FrontendIndexUnlock(&indices[0]); });
        gfxBuf.preTessIndexBuffer->used = 2049;
        fails([&] { KHZ_FrontendIndexUnlock(&indices[0]); });
        gfxBuf.preTessIndexBuffer->used = 0; check(KHZ_FrontendIndexUnlock(&indices[0]));
        role = 2; fails([&] { KHZ_FrontendUpload(frontEndDataOut); });
        role = 0; KHZ_FrontendUploadShutdown(); check(allocations.empty());
        // Reset, slot reuse, native frame wrap and zero-byte data.
        frontEndDataOut->skinnedCacheVb->used = 0; KHZ_FrontendUploadInit();
        for (unsigned frame : {0xffffffffu, 0u, 1u}) {
            select(0, frame); check(KHZ_FrontendVertexLock(&vertices[0][0], 0, 0, D3DLOCK_DISCARD, &p));
            frontEndDataOut->skinnedCacheVb->used = 0; check(KHZ_FrontendVertexUnlock(&vertices[0][0]));
            const unsigned prior = gpuCalls; role = 1; KHZ_FrontendUpload(frontEndDataOut); check(gpuCalls == prior);
        }
        role = 0; KHZ_FrontendUploadShutdown(); check(allocations.empty());
        std::printf("{\"passed\":true,\"checks\":%u,\"negative_controls\":%u,\"guard_pages_per_generation\":8,\"steady_frame_allocations\":0}\n", checks, rejected);
    } catch (const std::exception& e) { std::fprintf(stderr, "%s (check %u)\n", e.what(), checks); return 1; }
}
