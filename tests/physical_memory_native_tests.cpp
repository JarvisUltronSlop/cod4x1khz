#include <universal/q_shared.h>
#include <universal/physicalmemory.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
extern PhysicalMemory g_mem;
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("Production physical-memory assertion failed"); }
void Sys_OutOfMemErrorInternal(const char*, int) { throw std::runtime_error("Production physical-memory allocation rejected"); }
void Com_PrintError(int, const char*, ...) {}
void Com_Printf(int, const char*, ...) {}
double ConvertToMB(int) { throw std::runtime_error("Unexpected statistics service in allocator fixture"); }
char* va(const char*, ...) { static char text[] = "Unexpected hole in physical-memory allocations"; return text; }

int main() {
    PMem_Init();
    const auto capacity = PMem_GetFreeAmount();
    CHECK(capacity == (sizeof(void*) == 8 ? 0x40000000u : 0x08000000u));
    MEMORY_BASIC_INFORMATION page{};
    CHECK(VirtualQuery(g_mem.buf, &page, sizeof(page)));
    if constexpr (sizeof(void*) == 8) {
        CHECK(reinterpret_cast<uintptr_t>(g_mem.buf) > UINT32_MAX);
        CHECK(page.State == MEM_RESERVE);
    }
    const char firstName[] = "first", secondName[] = "second", highName[] = "high";
    PMem_BeginAlloc(firstName, 0);
    auto* first = PMem_Alloc(4097, 4096, 4, 0);
    CHECK(first == g_mem.buf);
    std::memset(first, 0x37, 4097);
    PMem_EndAlloc(firstName, 0);
    PMem_BeginAlloc(secondName, 0);
    auto* second = PMem_Alloc(17, 64, 4, 0);
    CHECK(second == g_mem.buf + 4160);
    std::memset(second, 0x81, 17);
    PMem_EndAlloc(secondName, 0);
    PMem_BeginAlloc(highName, 1);
    auto* high = PMem_Alloc(19, 4096, 4, 1);
    CHECK(high == g_mem.buf + capacity - 4096);
    std::memset(high, 0xb7, 19);
    PMem_EndAlloc(highName, 1);
    CHECK(first[4096] == 0x37 && second[16] == 0x81 && high[18] == 0xb7);
    CHECK(VirtualQuery(first, &page, sizeof(page)) && page.State == MEM_COMMIT);
    if constexpr (sizeof(void*) == 8)
        CHECK(VirtualQuery(g_mem.buf + capacity / 2, &page, sizeof(page)) && page.State == MEM_RESERVE);
    CHECK(PMem_GetFreeAmount() == capacity - 4096 - 4177);
    PMem_Free(secondName, 0);
    CHECK(g_mem.prim[0].pos == 4097 && g_mem.prim[0].allocListCount == 1);
    PMem_Free(firstName, 0);
    CHECK(g_mem.prim[0].pos == 0 && g_mem.prim[0].allocListCount == 0);
    PMem_Free(highName, 1);
    CHECK(PMem_GetFreeAmount() == capacity);
    PMem_BeginAlloc(firstName, 0);
    bool rejected = false;
    try { PMem_Alloc(UINT32_MAX, 4096, 4, 0); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected && g_mem.prim[0].pos == 0 && PMem_GetFreeAmount() == capacity);
    PMem_EndAlloc(firstName, 0); PMem_Free(firstName, 0);
    PMem_BeginAlloc(highName, 1); rejected = false;
    try { PMem_Alloc(UINT32_MAX, 4096, 4, 1); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected && g_mem.prim[1].pos == capacity && PMem_GetFreeAmount() == capacity);
    PMem_EndAlloc(highName, 1); PMem_Free(highName, 1);
    CHECK(VirtualFree(g_mem.buf, 0, MEM_RELEASE));
    std::printf("Production physical memory: %zu-bit arena, lazy commitment, aligned allocations, native bookkeeping and overflow rejection passed\n", sizeof(void*) * 8);
}
