#include <EffectsCore/fx_system.h>
#include <physics/phys_local.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <xanim/xmodel.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
const float fx_randomTable[507]{};
static XModel* model;
static PhysPreset preset{};
static dxBody* body = reinterpret_cast<dxBody*>(uintptr_t(sizeof(void*) == 8 ? UINT64_C(0x123456789000) : UINT64_C(0x12349000)));
static unsigned created, collision, angular, lockDepth;
static bool exhausted;
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("FX model assertion"); }
void Sys_EnterCriticalSection(int section) { CHECK(section == CRITSECT_PHYSICS && lockDepth == 0); ++lockDepth; }
void Sys_LeaveCriticalSection(int section) { CHECK(section == CRITSECT_PHYSICS && lockDepth == 1); --lockDepth; }
void FX_GetOrientation(const FxElemDef*, const FxSpatialFrame*, const FxSpatialFrame*, int32_t, orientation_t* value) { *value = {}; }
void FX_OrientationPosToWorldPos(const orientation_t*, const float* value, float* out) { std::memcpy(out, value, sizeof(float) * 3); }
void FX_GetElemAxis(const FxElemDef*, int32_t, const orientation_t*, float, mat3x3& value) { std::memset(value, 0, sizeof(value)); }
void AxisToQuat(const float (*)[3], float* out) { out[0] = out[1] = out[2] = 0; out[3] = 1; }
void FX_GetVelocityAtTime(const FxElemDef*, int32_t, float, float, const orientation_t*, const float* base, float* out) { std::memcpy(out, base, sizeof(float) * 3); }
FxElemVisuals FX_GetElemVisuals(const FxElemDef*, int32_t) { FxElemVisuals visuals{}; visuals.model = model; return visuals; }
dxBody* Phys_ObjCreate(PhysWorld world, float* position, float* quat, float* velocity, const PhysPreset* selected) {
    CHECK(world == PHYS_WORLD_FX && selected == &preset && lockDepth == 1);
    CHECK(position[0] == 42 && quat[3] == 1 && velocity[1] == 7);
    ++created; return exhausted ? nullptr : body;
}
void Phys_ObjSetCollisionFromXModel(const XModel* selected, PhysWorld world, dxBody* id) {
    CHECK(selected == model && world == PHYS_WORLD_FX && id == body && lockDepth == 1); ++collision;
}
void Phys_ObjSetAngularVelocity(dxBody* id, float* velocity) { CHECK(id == body && velocity[2] == 3000 && lockDepth == 1); ++angular; }
int main() {
    // The former pointer-index +53 read 424 bytes into a 280-byte x64 XModel.
    // Place its native layout against a guard page so that access cannot pass.
    auto* memory = static_cast<char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(memory && VirtualAlloc(memory, 4096, MEM_COMMIT, PAGE_READWRITE));
    model = reinterpret_cast<XModel*>(memory + 4096 - sizeof(XModel));
    std::memset(model, 0, sizeof(*model)); model->physPreset = &preset;
    FxEffect effect{}; FxElemDef definition{}; FxElem elem{};
    elem.origin[0] = 42; elem.baseVel[1] = 7;
    definition.lifeSpanMsec.base = 1000; definition.angularVelocity[2].base = 3;
    CHECK(FX_SpawnModelPhysics(nullptr, &effect, &definition, 0, &elem));
    CHECK(reinterpret_cast<dxBody*>(elem.physObjId) == body && created == 1 && collision == 1 && angular == 1 && lockDepth == 0);
    exhausted = true;
    // Spawning stores the body in the origin union; a new attempt has a fresh
    // element rather than the already converted physics element above.
    elem = {}; elem.origin[0] = 42; elem.baseVel[1] = 7;
    CHECK(!FX_SpawnModelPhysics(nullptr, &effect, &definition, 0, &elem));
    CHECK(elem.physObjId == 0 && created == 2 && collision == 1 && angular == 1 && lockDepth == 0);
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    std::puts("Native debris physics uses the model preset and preserves native body pointers");
}
