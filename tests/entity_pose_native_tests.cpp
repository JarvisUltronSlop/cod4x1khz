#include <cgame_mp/cg_local_mp.h>
#include <EffectsCore/fx_system.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #v); std::abort(); } } while (0)
static FxEffect* expectedEffect;
static int retired;
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
void FX_ThroughWithEffect(int client, FxEffect* effect) {
    CHECK(client == 0 && effect == expectedEffect && effect->def);
    // Simulate garbage collection after releasing the entity's last reference.
    effect->def = nullptr;
    ++retired;
}
int main() {
    auto* effect = static_cast<FxEffect*>(VirtualAlloc(nullptr, sizeof(FxEffect),
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    CHECK(effect);
    if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(effect) > UINT32_MAX);
    FxEffectDef definition{};
    expectedEffect = effect;
    for (int kind : {ET_FX, ET_LOOP_FX}) {
        centity_s entity{};
        entity.pose.eTypeUnion = kind;
        entity.pose.fx.effect = effect;
        entity.pose.fx.triggerTime = 12345;
        entity.pose.origin[0] = 42.0f;
        effect->def = &definition;
        const int before = retired;
        CG_ClearUnion(0, &entity);
        CHECK(retired == before + 1);
        CHECK(entity.pose.fx.effect == nullptr && entity.pose.fx.triggerTime == 0);
        CHECK(entity.pose.eTypeUnion == ET_GENERAL && entity.pose.origin[0] == 42.0f);
        entity.nextState.eType = kind;
        CG_SetUnionType(0, &entity);
        CHECK(entity.pose.eTypeUnion == kind && entity.pose.fx.effect == nullptr);
        CG_ClearUnion(0, &entity);
        CHECK(retired == before + 1); // No second release of a recycled effect.
    }
    for (int kind : {ET_PLAYER, ET_MG42, ET_HELICOPTER, ET_VEHICLE}) {
        centity_s entity{};
        entity.pose.eTypeUnion = kind;
        std::memset(&entity.pose.vehicle, 0xcd, sizeof(entity.pose.vehicle));
        CG_ClearUnion(0, &entity);
        const size_t cleared = kind == ET_PLAYER ? sizeof(CEntPlayerInfo) :
            kind == ET_MG42 ? sizeof(CEntTurretInfo) : sizeof(CEntVehicleInfo);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&entity.pose.vehicle);
        for (size_t i = 0; i < cleared; ++i) CHECK(bytes[i] == 0);
        CHECK(entity.pose.fx.effect == nullptr);
        CHECK(entity.pose.eTypeUnion == ET_GENERAL);
    }
    CHECK(VirtualFree(effect, 0, MEM_RELEASE));
    std::puts("Native entity pose retirement/reuse passed");
}
