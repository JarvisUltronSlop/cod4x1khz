#include <universal/q_shared.h>
#include <game/game_public.h>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
extern const game_hudelem_field_t fields_0[] = {
    {"archived", int32_t(offsetof(game_hudelem_s, archived)), F_INT, -1, 0, HudElem_SetBoolean, nullptr}
};
int scriptFlag;
int32_t Scr_GetInt(uint32_t index) { CHECK(index == 0); return scriptFlag; }

int main()
{
    std::array<game_hudelem_s, 2> hud{};
    hud[1].elem.type = HE_TYPE_TEXT;
    hud[1].clientNum = 7;
    hud[1].elem.x = 42;
    const auto next = hud[1];
    for (int flag : {0, 1, 0, 1}) {
        scriptFlag = flag;
        HudElem_SetBoolean(&hud[0], 0);
        CHECK(hud[0].archived == flag);
        CHECK(std::memcmp(&hud[1], &next, sizeof(next)) == 0);
    }
    std::puts("Native HUD flag writes preserve the adjacent live element");
}
