#include <universal/q_shared.h>
#include "r_rendercmds.h"
#include "r_init.h"

uint32_t s_renderCmdBufferSize;
int s_renderCmdWarnSize;
GfxCmdArray *s_cmdList;

GfxCmdHeader *__cdecl R_GetCommandBuffer(GfxRenderCommand renderCmd, int bytes)
{
    const char *v2; // eax
    GfxCmdHeader *header; // [esp+8h] [ebp-8h]
    int sizeLimit; // [esp+Ch] [ebp-4h]

    if ((uint32_t)renderCmd >= RC_COUNT)
        MyAssertHandler(
            ".\\r_rendercmds.cpp",
            881,
            0,
            "%s\n\t(renderCmd) = %i",
            "(renderCmd >= 0 && renderCmd < RC_COUNT)",
            renderCmd);
    iassert( bytes > 0 );
    bytes = (bytes + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
    iassert( (bytes < s_renderCmdBufferSize) );
    if (bytes != (uint16_t)bytes)
        MyAssertHandler(
            ".\\r_rendercmds.cpp",
            884,
            0,
            "%s\n\t(bytes) = %i",
            "(bytes == static_cast< unsigned short >( bytes ))",
            bytes);
    iassert( s_cmdList );
    iassert( s_cmdList->cmds );
    iassert( (s_cmdList->usedTotal & (sizeof(void *) - 1)) == 0 );
    iassert( rg.inFrame );
    if (renderCmd < RC_FIRST_NONCRITICAL && s_cmdList->usedCritical < 7680 && bytes + s_cmdList->usedCritical >= 7680)
        Com_PrintWarning(CON_CHANNEL_GFX, "RENDERCOMMAND_CRITICAL_WARN_SIZE (%i bytes) reached\n", 7680);
    if (s_cmdList->usedTotal < s_renderCmdWarnSize && bytes + s_cmdList->usedTotal >= s_renderCmdWarnSize)
        Com_PrintWarning(CON_CHANNEL_GFX, "RENDERCOMMAND_WARN_SIZE (%.0f KB) reached\n", (double)s_renderCmdWarnSize / 1024.0);
    sizeLimit = s_renderCmdBufferSize - s_cmdList->usedTotal;
    if (renderCmd >= RC_FIRST_NONCRITICAL)
        sizeLimit -= 0x2000 - s_cmdList->usedCritical;
    if (bytes <= sizeLimit)
    {
        header = (GfxCmdHeader *)&s_cmdList->cmds[s_cmdList->usedTotal];
        s_cmdList->usedTotal += bytes;
        s_cmdList->usedCritical += renderCmd >= RC_FIRST_NONCRITICAL ? 0 : bytes;
        s_cmdList->lastCmd = header;
        header->id = renderCmd;
        header->byteCount = bytes;
        return header;
    }
    else
    {
        if (renderCmd < RC_FIRST_NONCRITICAL)
        {
            v2 = va("rc %i used %i critical %i bytes %i", renderCmd, s_cmdList->usedTotal, s_cmdList->usedCritical, bytes);
            MyAssertHandler(".\\r_rendercmds.cpp", 904, 0, "%s\n\t%s", "renderCmd >= RC_FIRST_NONCRITICAL", v2);
        }
        s_cmdList->lastCmd = 0;
        return 0;
    }
}
