#include <universal/q_shared.h>
#include <DynEntity/DynEntity_client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
clipMap_t cm;
extern const DynEntityProps dynEntProps[3] = {
    { "invalid", false, false, false, false },
    { "clutter", true, false, true, false },
    { "destruct", true, false, true, true }
};
int32_t __cdecl DynEnt_CompareEntities(const void*, const void*);
static std::vector<byte> archive;
static std::vector<dxBody*> bodies;
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("Collision assertion"); }
void Com_Memset(void* destination, int value, size_t size) { std::memset(destination, value, size); }
void MemFile_WriteData(MemoryFile*, int size, const void* source) {
    CHECK(size >= 0);
    const auto* first = static_cast<const byte*>(source);
    archive.insert(archive.end(), first, first + size);
}
void Phys_ObjSave(dxBody* body, MemoryFile*) { bodies.push_back(body); }

int main() {
    // Unloading a map must retire the entire native collision structure,
    // including dynamic entity arrays beyond the former 284-byte boundary.
    static const char name[] = "maps/mp/mp_crash.d3dbsp";
    std::memset(&cm, 0xa5, sizeof(cm));
    cm.name = name;
    CM_Shutdown();
    clipMap_t empty{}; empty.name = name;
    CHECK(!std::memcmp(&cm, &empty, sizeof(cm)));
    CM_Shutdown();
    CHECK(!std::memcmp(&cm, &empty, sizeof(cm)));

    const uintptr_t base = sizeof(void*) == 8 ? UINT64_C(0x100000000) : UINT32_C(0x10000000);
    DynEntityDef defs[4]{};
    for (auto& def : defs) def.type = DYNENT_TYPE_CLUTTER;
    defs[0].xModel = reinterpret_cast<XModel*>(base * 2);
    defs[0].health = 222;
    defs[1].brushModel = 7; defs[1].health = 7;
    defs[2].xModel = reinterpret_cast<XModel*>(base);
    defs[2].health = 111;
    defs[3].brushModel = 2; defs[3].health = 2;
    std::qsort(defs, 4, sizeof(defs[0]), DynEnt_CompareEntities);
    CHECK(defs[0].brushModel == 2 && defs[0].health == 2);
    CHECK(defs[1].brushModel == 7 && defs[1].health == 7);
    CHECK(defs[2].xModel == reinterpret_cast<XModel*>(base) && defs[2].health == 111);
    CHECK(defs[3].xModel == reinterpret_cast<XModel*>(base * 2) && defs[3].health == 222);

    DynEntityPose poses[3]{};
    DynEntityClient clients[3]{};
    for (int i = 0; i < 3; ++i) {
        poses[i].pose.origin[0] = static_cast<float>(100 + i);
        poses[i].radius = static_cast<float>(10 + i);
        clients[i].physObjId = i == 1 ? 0 : base + 0x12340 + i * 0x100;
        clients[i].flags = DYNENT_CL_ACTIVE | DYNENT_CL_LINKED;
        clients[i].lightingHandle = static_cast<uint16_t>(0xab00 + i);
        clients[i].health = 700 + i;
    }
    cm.dynEntCount[0] = 2; cm.dynEntCount[1] = 1;
    cm.dynEntPoseList[0] = poses; cm.dynEntPoseList[1] = poses + 2;
    cm.dynEntClientList[0] = clients; cm.dynEntClientList[1] = clients + 2;
    MemoryFile file{};
    DynEnt_SaveEntities(&file);
    size_t cursor = 0;
    auto expect = [&](const void* source, size_t size) {
        CHECK(cursor + size <= archive.size());
        CHECK(!std::memcmp(archive.data() + cursor, source, size));
        cursor += size;
    };
    for (int type = 0; type < 2; ++type) {
        expect(&cm.dynEntCount[type], sizeof(cm.dynEntCount[type]));
        expect(cm.dynEntPoseList[type], sizeof(DynEntityPose) * cm.dynEntCount[type]);
        expect(cm.dynEntClientList[type], sizeof(DynEntityClient) * cm.dynEntCount[type]);
        for (int i = 0; i < cm.dynEntCount[type]; ++i) {
            const bool hasPhys = cm.dynEntClientList[type][i].physObjId != 0;
            expect(&hasPhys, sizeof(hasPhys));
        }
    }
    CHECK(cursor == archive.size() && bodies.size() == 2);
    CHECK(bodies[0] == reinterpret_cast<dxBody*>(clients[0].physObjId));
    CHECK(bodies[1] == reinterpret_cast<dxBody*>(clients[2].physObjId));
    CM_Shutdown();
    CHECK(!std::memcmp(&cm, &empty, sizeof(cm)));
    std::printf("Production collision shutdown and dynamic entity archive: %zu-bit passed\n", sizeof(void*) * 8);
}
