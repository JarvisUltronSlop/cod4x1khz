#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_add_staticmodel.h>
#include <gfx_d3d/r_draw_staticmodel.h>
#include <gfx_d3d/r_dobj_skin.h>
#include <gfx_d3d/r_debug.h>
#include <gfx_d3d/r_assetlists.h>
#include <gfx_d3d/rb_stats.h>
#include <gfx_d3d/r_staticmodelcache.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_hud_icon.h>
#include <unordered_set>
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
GfxBackEndData *frontEndDataOut;
static GfxPrimStats stats;
GfxPrimStats *g_primStats = &stats;
GfxFrameStats g_frameStatsCur;
r_globals_t rg;
r_global_permanent_t rgp;
extern static_model_cache_t s_cache;
void MyAssertHandler(const char *, int, int, const char *, ...) { throw std::runtime_error("Render command assertion"); }
void R_WarnOncePerFrame(GfxWarningType, ...) { throw std::runtime_error("Unexpected render command overflow"); }

int R_GetMaterialMemory(Material *) { return 321; }

extern GfxCmdArray *s_cmdList;
extern uint32_t s_renderCmdBufferSize;
extern int s_renderCmdWarnSize;
void Com_PrintWarning(int, const char *, ...) {}
char *QDECL va(const char *, ...) { static char text[] = "fixture overflow"; return text; }
void R_ConvertColorToBytes(const float *, uint32_t *color) { *color = 0xffffffff; }

int main() {
    alignas(void *) std::array<uint8_t, 65536> stream;
    GfxCmdArray commands{}; commands.cmds = stream.data(); s_cmdList = &commands;
    s_renderCmdBufferSize = stream.size(); s_renderCmdWarnSize = stream.size(); rg.inFrame = true;
    const float white[4] = {1, 1, 1, 1};
    auto *font = reinterpret_cast<Font_s *>(sizeof(void *) == 8 ? UINT64_C(0x123456780000) : 0x12340000);
    auto *material = reinterpret_cast<Material *>(sizeof(void *) == 8 ? UINT64_C(0x234567890000) : 0x23450000);
    for (unsigned length = 0; length < 258; ++length) {
        stream.fill(0xa5); commands.usedTotal = commands.usedCritical = 0;
        std::array<char, 260> text{}; memset(text.data(), 'A', length);
        auto *cmd = AddBaseDrawTextCmd(text.data(), 999, font, 1, 2, 1, 1, 0, white, 0, 0, '|'); CHECK(cmd);
        CHECK(cmd->font == font && !memcmp(cmd->text, text.data(), length + 1));
        CHECK(cmd->header.byteCount >= offsetof(GfxCmdDrawText2D, text) + length + 1);
        auto *next = reinterpret_cast<GfxCmdStretchPic *>(R_GetCommandBuffer(RC_STRETCH_PIC, sizeof(GfxCmdStretchPic)));
        CHECK(next && reinterpret_cast<uintptr_t>(next) % alignof(GfxCmdStretchPic) == 0);
        next->material = material; next->color.packed = 0x12345678;
        CHECK(next->material == material && !memcmp(cmd->text, text.data(), length + 1));
        for (size_t n = commands.usedTotal; n < stream.size(); ++n) CHECK(stream[n] == 0xa5);
    }
    std::array<char, 64> pool{};
    const int iconBytes = 1 + CONTXTCMD_LEN_HUDICON;
    std::array<char, 32> text{}; text[0] = '^'; text[1] = 1; text[2] = text[3] = 48;
    memcpy(text.data() + 4, &material, sizeof(material)); text[iconBytes] = 'X';
    for (int first = 0; first < 64; ++first) {
        for (int n = 0; n <= iconBytes; ++n) pool[(first + n) & 63] = text[n];
        stream.fill(0xa5); commands.usedTotal = commands.usedCritical = 0;
        auto *cmd = AddBaseDrawConsoleTextCmd(pool.data(), pool.size(), first, iconBytes + 1, font, 0, 0, 1, 1, white, 0);
        CHECK(cmd && !memcmp(cmd->text, text.data(), iconBytes + 2));
        CHECK(R_ReadHudIconMaterial(cmd->text + 1) == material);
        auto *next = R_GetCommandBuffer(RC_END_OF_LIST, sizeof(GfxCmdHeader)); CHECK(next);
        CHECK(!memcmp(cmd->text, text.data(), iconBytes + 2));
        for (size_t n = commands.usedTotal; n < stream.size(); ++n) CHECK(stream[n] == 0xa5);
    }
    rg.inFrame = false;

    for (unsigned bits = 4; bits <= 9; ++bits) {
        memset(&s_cache, 0, sizeof(s_cache)); SMC_ClearCache();
        std::unordered_set<uint16_t> allocated;
        const unsigned perPartition = 128 * (512 >> bits);
        for (unsigned partition = 0; partition < 4; ++partition) {
            for (unsigned n = 0; n < perPartition; ++n) {
                const auto id = SMC_Allocate(partition, bits); CHECK(id && allocated.insert(id).second);
                auto *leaf = SMC_GetLeaf(id); CHECK(leaf == &s_cache.leafs[(id - 1) / 32][(id - 1) % 32]);
                CHECK(leaf->cachedSurf.baseVertIndex == 16 * (id - 1));
                leaf->cachedSurf.smodelIndex = SMODEL_INDEX_NONE; leaf->cachedSurf.lodIndex = 0;
            }
            CHECK(!SMC_Allocate(partition, bits));
        }
        CHECK(allocated.size() == perPartition * 4);
        rg.frontEndFrameCount = 8;
        for (unsigned partition = 0; partition < 4; ++partition) {
            const auto recycled = SMC_Allocate(partition, bits); CHECK(recycled);
            SMC_GetLeaf(recycled)->cachedSurf.smodelIndex = SMODEL_INDEX_NONE;
        }
        R_FlushStaticModelCache(); rg.frontEndFrameCount = 0;
    }
    frontEndDataOut = static_cast<GfxBackEndData *>(VirtualAlloc(nullptr, sizeof(GfxBackEndData),
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE)); CHECK(frontEndDataOut);
    auto *surfaces = static_cast<XSurface *>(VirtualAlloc(nullptr, 65536,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE)); CHECK(surfaces);
    if constexpr (sizeof(void *) == 8) CHECK(reinterpret_cast<uintptr_t>(surfaces) > UINT32_MAX);
    surfaces[0].triCount = 13; surfaces[0].vertCount = 29;
    surfaces[1].triCount = 7; surfaces[1].vertCount = 18;
    std::array<uint16_t, 4> oddList{3, 40, 500, 0};
    std::array<uint16_t, 2> evenList{12, 75};
    GfxDelayedCmdBuf writer{}; R_InitDelayedCmdBuf(&writer);
    GfxDrawSurf key{}; key.packed = 123;
    std::array<GfxDrawSurf, 4> drawSurfaces{};
    GfxDrawSurfList list{drawSurfaces.data(), drawSurfaces.data() + drawSurfaces.size()};
    CHECK(R_AllocDrawSurf(&writer, key, &list, 3 + R_PRIM_DRAW_SURF_POINTER_WORDS));
    R_AddDelayedStaticModelDrawSurf(&writer, &surfaces[0], reinterpret_cast<uint8_t *>(oddList.data()), 3);
    CHECK(writer.primDrawSurfPos == 3 + R_PRIM_DRAW_SURF_POINTER_WORDS);
    CHECK(R_AllocDrawSurf(&writer, key, &list, 2 + R_PRIM_DRAW_SURF_POINTER_WORDS));
    R_AddDelayedStaticModelDrawSurf(&writer, &surfaces[1], reinterpret_cast<uint8_t *>(evenList.data()), 2);
    R_EndCmdBuf(&writer); CHECK(list.current == drawSurfaces.data() + 1);
    GfxStaticModelDrawStream reader{}; reader.primDrawSurfPos = frontEndDataOut->primDrawSurfsBuf;
    XSurface *out = nullptr;
    CHECK(R_GetNextStaticModelSurf(&reader, &out) && out == &surfaces[0] && reader.smodelCount == 3);
    CHECK(reader.smodelList[0] == 3 && reader.smodelList[1] == 40 && reader.smodelList[2] == 500);
    CHECK(R_GetNextStaticModelSurf(&reader, &out) && out == &surfaces[1] && reader.smodelCount == 2);
    CHECK(reader.smodelList[0] == 12 && reader.smodelList[1] == 75);
    CHECK(!R_GetNextStaticModelSurf(&reader, &out));
    CHECK(stats.dynamicIndexCount == 3 * (3 * 13 + 2 * 7));
    CHECK(stats.dynamicVertexCount == 3 * 29 + 2 * 18);
    CHECK(g_frameStatsCur.geoIndexCount == stats.dynamicIndexCount);
    CHECK(reader.primDrawSurfPos == frontEndDataOut->primDrawSurfsBuf + writer.primDrawSurfPos);

    // Real asset enumeration callbacks store full native pointers at both limits.
    struct GuardedModels { RModelList list{}; uint64_t guard = 0x0123456789abcdefULL; } models;
    struct GuardedMaterials { RMaterialList list{}; uint64_t guard = 0x0123456789abcdefULL; } materials;
    XAssetHeader asset(surfaces);
    R_GetModelList(asset, &models.list);
    CHECK(models.list.count == 1 && models.list.sorted[0] == asset.model);
    models.list.count = ARRAY_COUNT(models.list.sorted) - 1;
    R_GetModelList(asset, &models.list);
    CHECK(models.list.sorted[ARRAY_COUNT(models.list.sorted) - 1] == asset.model);
    bool rejected = false;
    try { R_GetModelList(asset, &models.list); } catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected && models.guard == 0x0123456789abcdefULL);
    R_GetMaterialList(asset, &materials.list);
    CHECK(materials.list.count == 1 && materials.list.sorted[0].material == asset.material);
    CHECK(materials.list.sorted[0].memory == 321);
    materials.list.count = ARRAY_COUNT(materials.list.sorted) - 1;
    R_GetMaterialList(asset, &materials.list);
    CHECK(materials.list.sorted[ARRAY_COUNT(materials.list.sorted) - 1].material == asset.material);
    rejected = false;
    try { R_GetMaterialList(asset, &materials.list); } catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected && materials.guard == 0x0123456789abcdefULL);

    // Native records remain aligned after a hidden surface marker.
    alignas(GfxModelRigidSurface) std::array<uint8_t, R_MODEL_SURF_SKIP_BYTES + sizeof(GfxModelRigidSurface)> records{};
    int hidden = -3; memcpy(records.data(), &hidden, sizeof(hidden));
    auto *rigid = reinterpret_cast<GfxModelRigidSurface *>(records.data() + R_MODEL_SURF_SKIP_BYTES);
    rigid->surf.xsurf = &surfaces[1]; rigid->surf.skinnedCachedOffset = -2;
    CHECK(reinterpret_cast<uintptr_t>(rigid) % alignof(GfxModelRigidSurface) == 0);
    CHECK(rigid->surf.xsurf == &surfaces[1]);
    CHECK(VirtualFree(surfaces, 0, MEM_RELEASE) && VirtualFree(frontEndDataOut, 0, MEM_RELEASE));
    std::printf("Production render commands: %zu-bit surface addresses, packed odd/even lists, sentinel, statistics and record alignment passed\n", sizeof(void *) * 8);
}
