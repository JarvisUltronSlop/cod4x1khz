#include <EffectsCore/fx_system.h>
#include <database/database.h>
#include <physics/phys_local.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
static FxSystem* currentSystem;
static FxSystemBuffers* currentBuffers;
static FxEffectDef* definitions[2];
static FxEffectDef* restored[2];
static std::vector<byte> archive;
static size_t cursor;
static bool writing;
static dvar_s fastFile;
const dvar_s* useFastFile = &fastFile;
const float fx_randomTable[507]{};
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("FX archive assertion"); }
void QDECL Com_Error(errorParm_t, const char*, ...) { throw std::runtime_error("FX archive error"); }
char* QDECL va(const char*, ...) { static char text[] = "FX fixture assertion"; return text; }
FxSystem* FX_GetSystem(int) { return currentSystem; }
FxSystemBuffers* FX_GetSystemBuffers(int) { return currentBuffers; }
const FxEffectDef* FX_Register(const char* name) {
    for (int i = 0; i < 2; ++i) if (!std::strcmp(name, restored[i]->name)) return restored[i];
    throw std::runtime_error("Unexpected effect registration");
}
void FX_ForEachEffectDef(void(__cdecl* callback)(const FxEffectDef*, void*), void* context) {
    for (auto* effect : definitions) callback(effect, context);
}
void DB_EnumXAssets(XAssetType, void(__cdecl* callback)(XAssetHeader, void*), void* context, bool) {
    for (auto* effect : definitions) { XAssetHeader header{}; header.fx = effect; callback(header, context); }
}
void Phys_ObjSave(dxBody*, MemoryFile*) { throw std::runtime_error("Unexpected physics object"); }
dxBody* Phys_ObjLoad(PhysWorld, MemoryFile*) { throw std::runtime_error("Unexpected physics object"); }
void Phys_ObjSetCollisionFromXModel(const XModel*, PhysWorld, dxBody*) { throw std::runtime_error("Unexpected physics object"); }
bool MemFile_IsWriting(MemoryFile*) { return writing; }
int MemFile_GetUsedSize(MemoryFile*) { return static_cast<int>(archive.size()); }
void MemFile_WriteData(MemoryFile*, int count, const void* source) {
    CHECK(writing && count >= 0); const auto* first = static_cast<const byte*>(source); archive.insert(archive.end(), first, first + count);
}
void MemFile_ReadData(MemoryFile*, int count, byte* destination) {
    CHECK(!writing && count >= 0 && count <= archive.size() - cursor); std::memcpy(destination, archive.data() + cursor, count); cursor += count;
}
void MemFile_WriteCString(MemoryFile* file, const char* text) { MemFile_WriteData(file, static_cast<int>(std::strlen(text) + 1), text); }
const char* MemFile_ReadCString(MemoryFile*) {
    CHECK(cursor < archive.size()); auto* text = reinterpret_cast<const char*>(archive.data() + cursor);
    size_t length = 0; while (cursor + length < archive.size() && text[length]) ++length;
    CHECK(cursor + length < archive.size()); cursor += length + 1; return text;
}

struct Fixture { FxSystem system; FxSystemBuffers buffers; };
static Fixture* allocate(uintptr_t address) {
    auto* memory = static_cast<Fixture*>(VirtualAlloc(reinterpret_cast<void*>(address), sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(memory); if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(memory) > UINT32_MAX); return memory;
}
int main() {
    fastFile.current.enabled = true;
    auto* oldState = allocate(sizeof(void*) == 8 ? UINT64_C(0x100010000) : 0);
    auto* newState = allocate(sizeof(void*) == 8 ? UINT64_C(0x300010000) : 0);
    definitions[0] = static_cast<FxEffectDef*>(VirtualAlloc(sizeof(void*) == 8 ? reinterpret_cast<void*>(UINT64_C(0x500010000)) : nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    definitions[1] = static_cast<FxEffectDef*>(VirtualAlloc(sizeof(void*) == 8 ? reinterpret_cast<void*>(UINT64_C(0x600010000)) : nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(definitions[0] && definitions[1]);
    FxEffectDef newDefs[2]{}; restored[0] = &newDefs[0]; restored[1] = &newDefs[1];
    for (int i = 0; i < 2; ++i) definitions[i]->name = restored[i]->name = i ? "fixture-b" : "fixture-a";
    if constexpr (sizeof(void*) == 8) CHECK(uint32_t(reinterpret_cast<uintptr_t>(definitions[0])) == uint32_t(reinterpret_cast<uintptr_t>(definitions[1])));
    currentSystem = &oldState->system; currentBuffers = &oldState->buffers;
    FX_LinkSystemBuffers(currentSystem, currentBuffers);
    currentSystem->isInitialized = true; currentSystem->firstActiveEffect = 0; currentSystem->firstNewEffect = 2;
    currentSystem->visStateBufferRead = &currentBuffers->visState[1]; currentSystem->visStateBufferWrite = &currentBuffers->visState[0];
    for (int i = 0; i < MAX_EFFECTS; ++i) {
        const auto handle = FX_EffectToHandle(currentSystem, &currentBuffers->effects[i]);
        CHECK(handle >= 0 && handle < FX_HANDLE_NONE && FX_EffectFromHandle(currentSystem, static_cast<uint16_t>(handle)) == &currentBuffers->effects[i]);
        currentSystem->allEffectHandles[i] = static_cast<uint16_t>(handle);
    }
    for (int i = 0; i < 2; ++i) {
        auto& effect = currentBuffers->effects[i]; effect.def = definitions[i]; effect.firstElemHandle[1] = FX_HANDLE_NONE;
        effect.frameNow.origin[0] = 10 + i;
    }
    currentBuffers->padBuffer[95] = 0xa5; currentSystem->restartList[31] = 0x12345678;
    MemoryFile file{}; writing = true; FX_Archive(0, &file); CHECK(!currentSystem->isArchiving);
    CHECK(archive.size() > sizeof(FxSystem) + sizeof(FxSystemBuffers));
    currentSystem = &newState->system; currentBuffers = &newState->buffers;
    writing = false; FX_Archive(0, &file);
    CHECK(cursor == archive.size() && currentSystem->isInitialized && !currentSystem->isArchiving);
    CHECK(currentSystem->effects == currentBuffers->effects && currentSystem->restartList[31] == 0x12345678);
    CHECK(currentSystem->visStateBufferRead == &currentBuffers->visState[1] && currentSystem->visStateBufferWrite == &currentBuffers->visState[0]);
    for (int i = 0; i < 2; ++i) CHECK(currentBuffers->effects[i].def == restored[i] && currentBuffers->effects[i].frameNow.origin[0] == 10 + i);
    CHECK(currentBuffers->padBuffer[95] == 0xa5);
    CHECK(VirtualFree(oldState, 0, MEM_RELEASE) && VirtualFree(newState, 0, MEM_RELEASE));
    for (auto* effect : definitions) CHECK(VirtualFree(effect, 0, MEM_RELEASE));
    std::printf("Production FX archive: %zu-bit keys, complete buffers, all handles and relocation beyond 4 GiB passed\n", sizeof(void*) * 8);
}
