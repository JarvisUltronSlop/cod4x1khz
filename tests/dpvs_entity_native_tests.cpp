#include <universal/q_shared.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_bsp.h>
#include <Windows.h>
#include <array>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
GfxScene scene;
GfxConfiguration gfxCfg;
DpvsGlobals dpvsGlob;
r_global_permanent_t rgp;
std::vector<void*> allocations;
bool Sys_IsMainThread() { return true; }
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("DPVS assertion"); }
void Com_Memset(void* destination, int value, size_t bytes) { std::memset(destination, value, bytes); }
char* R_AllocGlobalVariable(uint32_t bytes, const char*) {
    const size_t pages = (bytes + 4095u) & ~size_t(4095u);
    auto* allocation = static_cast<char*>(VirtualAlloc(nullptr, pages + 4096, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(allocation && VirtualAlloc(allocation, pages, MEM_COMMIT, PAGE_READWRITE));
    allocations.push_back(allocation);
    return allocation + pages - bytes;
}
int main() {
    gfxCfg.entCount = 1024;
    gfxCfg.maxClientViews = 1;
    R_InitSceneBuffers();
    static GfxWorld world{};
    world.dpvsPlanes.cellCount = 1;
    std::array<uint32_t, 256> bits{};
    world.dpvsPlanes.sceneEntCellBits = bits.data();
    rgp.world = &world;
    auto* info = scene.dpvs.entInfo[0];
    // The last half of these entries crossed the former x64 allocation into
    // other renderer buffers. A guard page turns that corruption into a failure.
    for (unsigned ent = 0; ent < gfxCfg.entCount; ++ent) {
        FilterEntInfo entry{};
        entry.entnum = ent;
        entry.info.bmodel = reinterpret_cast<GfxBrushModel*>(
            (sizeof(void*) == 8 ? UINT64_C(0x123456780000) : UINT64_C(0x12340000)) + ent * 16);
        R_AddEntToCell(&entry, 0);
        CHECK(info[ent].bmodel == entry.info.bmodel);
    }
    CHECK(bits[0] == UINT32_MAX && bits[31] == UINT32_MAX);
    R_InitSceneData(0);
    for (unsigned ent = 0; ent < gfxCfg.entCount; ++ent) CHECK(info[ent].bmodel == nullptr);
    for (auto word : bits) CHECK(word == 0);
    for (auto* allocation : allocations) CHECK(VirtualFree(allocation, 0, MEM_RELEASE));
    std::puts("Native DPVS entity allocation, high IDs and full clearing passed");
}
