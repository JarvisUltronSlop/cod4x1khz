#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include "../native/ticket_mutex.hpp"
#include <thread>
#include <atomic>
#include <khz/khz_controls.h>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
static std::unordered_set<const char*> allocations;
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("Production dvar assertion failed"); }
const char* CopyString(const char* text) {
    auto* copy = static_cast<char*>(std::malloc(std::strlen(text) + 1)); CHECK(copy);
    std::strcpy(copy, text); allocations.insert(copy); return copy;
}
void FreeString(const char* text) { CHECK(allocations.erase(text) == 1); std::free(const_cast<char*>(text)); }
int I_stricmp(const char* first, const char* second) { return _stricmp(first, second); }
int I_strnicmp(const char* first, const char* second, int count) { return _strnicmp(first, second, count); }

int main() {
    std::atomic<bool> lookedUp{false};
    KHZ_DvarEnter(); KHZ_DvarEnter();
    std::thread lookupDuringTick([&] { KHZ_DvarLookupScope lookup; lookedUp.store(true); });
    const auto lookupDeadline = GetTickCount64() + 1000;
    while (!lookedUp.load()) { CHECK(GetTickCount64() < lookupDeadline); SwitchToThread(); }
    lookupDuringTick.join(); KHZ_DvarLeave(); KHZ_DvarLeave();
    KHZ_DvarRegistryEnter(); KHZ_DvarRegistryEnter();
    { KHZ_DvarLookupScope nestedWriterLookup; }
    KHZ_DvarRegistryLeave();
    lookedUp = false;
    std::atomic<bool> waiting{false};
    std::thread blockedLookup([&] { waiting.store(true); KHZ_DvarLookupScope lookup; lookedUp.store(true); });
    while (!waiting.load()) SwitchToThread();
    Sleep(20); CHECK(!lookedUp.load());
    KHZ_DvarRegistryLeave(); blockedLookup.join(); CHECK(lookedUp.load());
    khz::TicketMutex mutex;
    std::array<unsigned, 2> order{};
    std::atomic<unsigned> entered{0};
    mutex.lock();
    std::thread firstWaiter([&] { mutex.lock(); order[entered.fetch_add(1)] = 1; mutex.unlock(); });
    const auto until = GetTickCount64() + 5000;
    while (mutex.pending() < 2) { CHECK(GetTickCount64() < until); SwitchToThread(); }
    std::thread secondWaiter([&] { mutex.lock(); order[entered.fetch_add(1)] = 2; mutex.unlock(); });
    while (mutex.pending() < 3) { CHECK(GetTickCount64() < until); SwitchToThread(); }
    mutex.unlock();
    firstWaiter.join(); secondWaiter.join();
    CHECK(entered == 2 && order[0] == 1 && order[1] == 2 && mutex.pending() == 0);
    unsigned protectedValue = 0;
    std::array<std::thread, 4> contenders;
    for (auto& contender : contenders) contender = std::thread([&] {
        for (unsigned pass = 0; pass < 2000; ++pass) { mutex.lock(); ++protectedValue; mutex.unlock(); }
    });
    for (auto& contender : contenders) contender.join();
    CHECK(protectedValue == 8000 && mutex.pending() == 0);
    auto* first = static_cast<char*>(VirtualAlloc(sizeof(void*) == 8 ? reinterpret_cast<void*>(UINT64_C(0x100010000)) : nullptr,
        65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); CHECK(first);
    auto* second = static_cast<char*>(VirtualAlloc(sizeof(void*) == 8 ? reinterpret_cast<void*>(UINT64_C(0x200010000)) : nullptr,
        65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); CHECK(second);
    std::strcpy(first, "alpha"); std::strcpy(second, "beta");
    if constexpr (sizeof(void*) == 8) {
        CHECK(reinterpret_cast<uintptr_t>(first) > UINT32_MAX);
        CHECK(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(first)) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(second)));
    }
    dvar_s dvar{}; dvar.type = DVAR_TYPE_STRING; dvar.name = "native fixture";
    dvar.current.string = first; dvar.latched.string = second; dvar.reset.string = second;
    CHECK(Dvar_ShouldFreeCurrentString(&dvar));
    CHECK(!Dvar_ShouldFreeLatchedString(&dvar) && !Dvar_ShouldFreeResetString(&dvar));
    Dvar_CopyString(first, &dvar.current); Dvar_WeakCopyString(dvar.current.string, &dvar.latched);
    Dvar_WeakCopyString(dvar.current.string, &dvar.reset);
    Dvar_SetLatchedValue(&dvar, DvarValue(second));
    CHECK(!std::strcmp(dvar.latched.string, second) && dvar.current.string != dvar.latched.string);
    Dvar_UpdateValue(&dvar, DvarValue(second));
    CHECK(dvar.current.string == dvar.latched.string && !std::strcmp(dvar.reset.string, first));
    Dvar_UpdateResetValue(&dvar, DvarValue(second));
    CHECK(dvar.current.string == dvar.reset.string && allocations.size() == 1);
    Dvar_SetLatchedValue(&dvar, DvarValue(first));
    CHECK(allocations.size() == 2);
    if (Dvar_ShouldFreeCurrentString(&dvar)) Dvar_FreeString(&dvar.current);
    dvar.current.string = nullptr;
    if (Dvar_ShouldFreeResetString(&dvar)) Dvar_FreeString(&dvar.reset);
    dvar.reset.string = nullptr;
    if (Dvar_ShouldFreeLatchedString(&dvar)) Dvar_FreeString(&dvar.latched);
    dvar.latched.string = nullptr; CHECK(allocations.empty());
    std::array<const char*, 2> table{first, second};
    dvar.type = DVAR_TYPE_ENUM; dvar.domain.enumeration = {2, table.data()}; dvar.current.integer = 1;
    CHECK(Dvar_EnumToString(&dvar) == second);
    CHECK(Dvar_IndexStringToEnumString(&dvar, "0") == first);
    CHECK(!std::strcmp(Dvar_IndexStringToEnumString(&dvar, "2"), ""));
    CHECK(Dvar_StringToEnum(&dvar.domain, "BETA") == 1);
    CHECK(Dvar_StringToEnum(&dvar.domain, "0") == 0 && Dvar_StringToEnum(&dvar.domain, "missing") == -1337);
    CHECK(DvarValue(17).integer == 17 && DvarLimits(-3, 9).integer.max == 9);
    CHECK(VirtualFree(first, 0, MEM_RELEASE) && VirtualFree(second, 0, MEM_RELEASE));
    std::printf("Production dvar values: %zu-bit string ownership, colliding low address bits, enum tables and numeric domains passed\n", sizeof(void*) * 8);
}
