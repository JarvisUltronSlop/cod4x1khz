#include <universal/q_shared.h>
#include "r_draw_staticmodel.h"
#include "rb_stats.h"

int __cdecl R_GetNextStaticModelSurf(GfxStaticModelDrawStream *drawStream, XSurface **outSurf)
{
    XSurface *xsurf; // [esp+0h] [ebp-Ch]
    const uint32_t *primDrawSurfPos; // [esp+4h] [ebp-8h]

    drawStream->smodelCount = *drawStream->primDrawSurfPos++;
    if (!drawStream->smodelCount)
        return 0;
    primDrawSurfPos = drawStream->primDrawSurfPos;
    xsurf = R_ReadPrimDrawSurfXSurface(primDrawSurfPos);
    drawStream->smodelList = reinterpret_cast<const uint16_t *>(primDrawSurfPos);
    drawStream->primDrawSurfPos = primDrawSurfPos + ((drawStream->smodelCount + 1) >> 1);
    drawStream->localSurf = xsurf;
    g_frameStatsCur.geoIndexCount += 3 * drawStream->smodelCount * xsurf->triCount;

    iassert(g_primStats);
    g_primStats->dynamicIndexCount += 3 * drawStream->smodelCount * xsurf->triCount;
    g_primStats->dynamicVertexCount += drawStream->smodelCount * xsurf->vertCount;
    *outSurf = xsurf;
    return 1;
}
