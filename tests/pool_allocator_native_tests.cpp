#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <universal/pool_allocator.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }

int main() {
    constexpr unsigned Stride = 32, Capacity = 64;
    auto* memory = static_cast<char*>(VirtualAlloc(nullptr, 4096,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    CHECK(memory);
#if defined(_WIN64)
    CHECK(reinterpret_cast<std::uintptr_t>(memory) > UINT32_MAX);
#endif
    for (unsigned count : {2u, 17u, Capacity}) {
        std::memset(memory, 0xa5, 4096);
        pooldata_t pool{};
        Pool_Init(memory, &pool, Stride, count);
        CHECK(pool.activeCount == 0 && Pool_FreeCount(&pool) == count);
        std::array<freenode*, Capacity> entries{};
        for (unsigned i = 0; i < count; ++i) {
            entries[i] = Pool_Alloc(&pool);
            CHECK(entries[i] == reinterpret_cast<freenode*>(memory + Stride * i));
            CHECK(pool.activeCount == static_cast<int>(i + 1));
            CHECK(Pool_FreeCount(&pool) == count - i - 1);
            for (unsigned byte = sizeof(void*); byte < Stride; ++byte)
                CHECK(static_cast<unsigned char>(memory[Stride * i + byte]) == 0xa5);
        }
        CHECK(Pool_Alloc(&pool) == nullptr && pool.activeCount == static_cast<int>(count));
        for (unsigned i = 0; i < count; ++i) Pool_Free(entries[i], &pool);
        CHECK(pool.activeCount == 0 && Pool_FreeCount(&pool) == count);
        for (unsigned i = 0; i < count; ++i) CHECK(Pool_Alloc(&pool) == entries[count - i - 1]);
        for (unsigned i = count; i < Capacity; ++i)
            for (unsigned byte = 0; byte < Stride; ++byte)
                CHECK(static_cast<unsigned char>(memory[Stride * i + byte]) == 0xa5);
    }
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    std::printf("Native pool: process_bits=%zu address=%llx; allocation, exhaustion, reuse and canaries passed\n",
        sizeof(void*) * 8, static_cast<unsigned long long>(address));
}
