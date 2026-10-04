#include <universal/com_memory.h>
#include <qcommon/threads.h>
#include <script/scr_stringlist.h>
#include <qcommon/cmd.h>
#include <database/database.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("Production hunk assertion failed"); }
bool Sys_IsMainThread() { return true; }
void Sys_OutOfMemErrorInternal(const char*, int) { throw std::runtime_error("Fixture could not commit native hunk memory"); }
void Com_Error(errorParm_t, const char*, ...) { throw std::runtime_error("Production hunk rejected allocation"); }
void track_PrintAllInfo() {}
void track_hunk_alloc(int, int, const char*, int) {}
void track_hunk_allocLow(int, int, const char*, int) {}
void track_temp_high_alloc(int, int, int, const char*) {}
void track_temp_high_clear(int) {}
void track_temp_alloc(int, int, int, const char*) {}
void track_temp_free(int, int, const char*) {}
void track_temp_clear(int) {}
void track_z_commit(int, int) {}
void track_set_hunk_size(int) {}
void track_hunk_ClearToMarkHigh(int) {}
void track_hunk_ClearToMarkLow(int) {}
void track_hunk_ClearToStart() {}
void track_PrintInfo() {}
void Com_Memset(void* target, int value, size_t bytes) { std::memset(target, value, bytes); }
void Com_Printf(int, const char*, ...) {}

// These services belong to unrelated routines in the production memory object.
// Fail if the allocator fixture accidentally starts depending on them.
[[noreturn]] static void unusedService() { throw std::runtime_error("Unexpected game service in allocator fixture"); }
int I_stricmp(const char*, const char*) { unusedService(); }
unsigned Sys_Milliseconds() { unusedService(); }
unsigned SL_GetString_(const char*, unsigned, mtType_t) { unusedService(); }
const char* SL_ConvertToString(unsigned) { unusedService(); }
unsigned SL_FindString(const char*) { unusedService(); }
void SL_RemoveRefToString(unsigned) { unusedService(); }
struct XModelPartsLoad; struct XAnim_s; struct XAnimParts;
void XModelPartsFree(XModelPartsLoad*) { unusedService(); }
void XAnimFreeList(XAnim_s*) { unusedService(); }
void XAnimFree(XAnimParts*) { unusedService(); }
bool Sys_IsRenderThread() { unusedService(); }
int FS_LoadStack() { unusedService(); }
int FS_HashFileName(const char*, int) { unusedService(); }
void Cmd_AddCommandInternal(const char*, void(*)(), cmd_function_s*) { unusedService(); }
void R_ReflectionProbeRegisterDvars() { unusedService(); }
void PMem_DumpMemStats() { unusedService(); }
const dvar_t* useFastFile = nullptr;
const dvar_t* r_reflectionProbeGenerate = nullptr;
fileData_s* com_fileDataHashTable[1024]{};

int main() {
    auto* user = Hunk_UserCreate(16384, "native fixture", true, false, 0);
    CHECK(reinterpret_cast<uintptr_t>(user->buf) % 32 == 0);
    if constexpr (sizeof(void*) == 8) CHECK(user->pos > UINT32_MAX);
    const auto begin = user->pos;
    auto* first = static_cast<std::uint8_t*>(Hunk_UserAlloc(user, 73, 32));
    auto* second = static_cast<std::uint8_t*>(Hunk_UserAlloc(user, 6145, 4096));
    CHECK(first == user->buf && reinterpret_cast<uintptr_t>(second) % 4096 == 0);
    std::memset(first, 0x37, 73); std::memset(second, 0x81, 6145);
    CHECK(first[72] == 0x37 && second[6144] == 0x81);
    Hunk_UserSetPos(user, second + 17);
    CHECK(Hunk_UserAllocAlignStrict(user, 5) == second + 17);
    Hunk_UserReset(user); CHECK(user->pos == begin && user->name && user->maxSize == 16384);
    CHECK(user->buf[0] == 0 && user->buf[4095 - offsetof(HunkUser, buf)] == 0);
    MEMORY_BASIC_INFORMATION page{};
    CHECK(VirtualQuery(reinterpret_cast<std::uint8_t*>(user) + 4096, &page, sizeof(page)));
    CHECK(page.State == MEM_RESERVE);
    Hunk_UserDestroy(user);

    // Flexible users grow into another independently owned virtual allocation.
    user = Hunk_UserCreate(8192, "native chained fixture", false, false, 0);
    Hunk_UserAlloc(user, 7000, 32);
    auto* chained = Hunk_UserAlloc(user, 7000, 32);
    CHECK(user->next && user->current == user->next);
    CHECK(reinterpret_cast<uintptr_t>(chained) >= reinterpret_cast<uintptr_t>(user->next->buf));
    Hunk_UserReset(user); CHECK(!user->next && user->current == user);
    Hunk_UserDestroy(user);

    s_hunkTotal = 8 * 1024 * 1024;
    s_hunkData = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, s_hunkTotal,
        MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE)); CHECK(s_hunkData);
    if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(s_hunkData) > UINT32_MAX);
    std::array<XAssetHeader, 3> assetSlots{};
    AssetList assets{0, static_cast<int>(assetSlots.size()), assetSlots.data()};
    XAssetHeader asset{}; asset.data = s_hunkData;
    for (unsigned i = 0; i < assetSlots.size(); ++i) Hunk_AddAsset(asset, &assets);
    CHECK(assets.assetCount == 3 && assetSlots[2].data == s_hunkData);
    bool rejected = false;
    try { Hunk_AddAsset(asset, &assets); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected && assets.assetCount == 3);
    auto* low = Hunk_AllocLowAlign(513, 32, "native low", 0);
    auto* high = Hunk_AllocAlign(713, 64, "native high", 0);
    CHECK(low == s_hunkData && reinterpret_cast<uintptr_t>(high) % 64 == 0);
    std::memset(low, 0x29, 513); std::memset(high, 0x62, 713);
    auto* temp = reinterpret_cast<std::uint8_t*>(Hunk_AllocateTempMemory(8193, "native low temp"));
    CHECK(reinterpret_cast<uintptr_t>(temp) % 16 == 0); std::memset(temp, 0xab, 8193);
    CHECK(low[512] == 0x29 && high[712] == 0x62);
    Hunk_FreeTempMemory(reinterpret_cast<char*>(temp));
    auto* tempHigh = Hunk_AllocateTempMemoryHigh(8193, "native high temp");
    CHECK(reinterpret_cast<uintptr_t>(tempHigh) % 16 == 0); std::memset(tempHigh, 0xcd, 8193);
    CHECK(low[512] == 0x29 && high[712] == 0x62);
    Hunk_ClearTempMemoryHigh();
    CHECK(VirtualFree(s_hunkData, 0, MEM_RELEASE)); s_hunkData = nullptr; s_hunkTotal = 0;
    std::printf("Production hunk memory: %zu-bit addresses, page commits, alignment, temporary headers, reset and growth passed\n", sizeof(void*) * 8);
}
