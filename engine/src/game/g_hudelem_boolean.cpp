#include <universal/q_shared.h>
#include "game_public.h"
#include <script/scr_vm.h>

extern const game_hudelem_field_t fields_0[];

void __cdecl HudElem_SetBoolean(game_hudelem_s *hud, int32_t offset)
{
    // Script values contain native pointers on x64. HUD flags remain four-byte
    // fields; storing a VariableUnion here overwrites the next HUD element.
    *reinterpret_cast<int32_t *>(reinterpret_cast<char *>(hud) + fields_0[offset].ofs) = Scr_GetInt(0);
}
