#include <universal/q_shared.h>
#include "r_rendercmds.h"
#include "r_utils.h"
#include <universal/profile.h>

GfxCmdDrawText2D *__cdecl AddBaseDrawTextCmd(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    int cursorPos,
    char cursor)
{
    uint32_t v13; // [esp+0h] [ebp-4Ch]
    GfxCmdDrawText2D *cmd; // [esp+48h] [ebp-4h]

    iassert( maxChars > 0 );
    iassert( text );
    if (!*text && cursorPos < 0)
        return 0;
    v13 = strlen(text);
    cmd = (GfxCmdDrawText2D *)R_GetCommandBuffer(RC_DRAW_TEXT_2D, offsetof(GfxCmdDrawText2D, text) + v13 + 1);
    if (!cmd)
        return 0;
    cmd->x = x;
    cmd->y = y;
    cmd->rotation = rotation;
    cmd->font = font;
    cmd->xScale = xScale;
    cmd->yScale = yScale;
    R_ConvertColorToBytes(color, (uint32_t *)&cmd->color);
    cmd->maxChars = maxChars;
    cmd->renderFlags = 0;
    switch (style)
    {
    case 3:
        cmd->renderFlags |= 4u;
        break;
    case 6:
        cmd->renderFlags |= 0xCu;
        break;
    case 128:
        cmd->renderFlags |= 1u;
        break;
    }
    if (cursorPos > -1)
    {
        cmd->renderFlags |= 2u;
        cmd->cursorPos = cursorPos;
        cmd->cursorLetter = cursor;
    }
    {
        PROF_SCOPED("R_memcpy");
        memcpy((uint8_t *)cmd->text, (uint8_t *)text, v13);
    }
    cmd->text[v13] = 0;
    return cmd;
}

GfxCmdDrawText2D *__cdecl AddBaseDrawConsoleTextCmd(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style)
{
    GfxCmdDrawText2D *cmd; // [esp+4h] [ebp-4h]

    iassert( charCount >= 0 );
    iassert( textPool );
    if (!charCount)
        return 0;
    cmd = (GfxCmdDrawText2D *)R_GetCommandBuffer(RC_DRAW_TEXT_2D, offsetof(GfxCmdDrawText2D, text) + charCount + 1);
    if (!cmd)
        return 0;
    cmd->x = x;
    cmd->y = y;
    cmd->rotation = 0.0;
    cmd->font = font;
    cmd->xScale = xScale;
    cmd->yScale = yScale;
    R_ConvertColorToBytes(color, (uint8_t *)&cmd->color);
    cmd->maxChars = 0x7FFFFFFF;
    cmd->renderFlags = 0;
    switch (style)
    {
    case 3:
        cmd->renderFlags |= 4u;
        break;
    case 6:
        cmd->renderFlags |= 0xCu;
        break;
    case 128:
        cmd->renderFlags |= 1u;
        break;
    }
    CopyPoolTextToCmd(textPool, poolSize, firstChar, charCount, cmd);
    return cmd;
}

void __cdecl CopyPoolTextToCmd(char *textPool, int poolSize, int firstChar, int charCount, GfxCmdDrawText2D *cmd)
{
    uint32_t poolRemaining; // [esp+30h] [ebp-4h]

    iassert(cmd);

    PROF_SCOPED("R_memcpy");
    poolRemaining = poolSize - firstChar;

    if (charCount > poolSize - firstChar)
    {
        memcpy((uint8_t *)cmd->text, (uint8_t *)&textPool[firstChar], poolRemaining);
        memcpy((uint8_t *)&cmd->text[poolRemaining], (uint8_t *)textPool, charCount - poolRemaining);
    }
    else
    {
        memcpy((uint8_t *)cmd->text, (uint8_t *)&textPool[firstChar], charCount);
    }

    cmd->text[charCount] = 0;
}
