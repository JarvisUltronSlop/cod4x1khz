#include <universal/q_shared.h>
#include "qcommon.h"
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include <khz/khz_server_collision_checkpoint.h>
#endif

void __cdecl CM_Shutdown()
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowUnsupportedEffect("server_collision_map_shutdown")) return;
    if (!KHZ_ServerCollisionInvalidateMapBinding()) return;
#endif
    const char *savedName; // [esp+0h] [ebp-4h]

    savedName = cm.name;
    Com_Memset(&cm, 0, sizeof(cm));
    cm.name = savedName;
    iassert( !cm.isInUse );
}
