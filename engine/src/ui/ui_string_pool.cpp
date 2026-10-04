#include <universal/q_shared.h>
#include "ui_shared.h"
#include <qcommon/threads.h>

stringDef_s *g_strHandle[2048];

void __cdecl String_Init()
{
    memset((uint8_t *)g_strHandle, 0, sizeof(g_strHandle));
}

const char *staticNULL = "";
const char *__cdecl String_Alloc(const char *p)
{
    char v2; // [esp+3h] [ebp-45h]
    uint8_t *v3; // [esp+8h] [ebp-40h]
    const char *v4; // [esp+Ch] [ebp-3Ch]
    stringDef_s *str; // [esp+34h] [ebp-14h]
    stringDef_s *stra; // [esp+34h] [ebp-14h]
    stringDef_s *strb; // [esp+34h] [ebp-14h]
    int hash; // [esp+38h] [ebp-10h]
    uint8_t *s; // [esp+40h] [ebp-8h]
    stringDef_s *last; // [esp+44h] [ebp-4h]

    if (!Sys_IsMainThread())
        MyAssertHandler(".\\ui\\ui_utils.cpp", 458, 0, "%s", "Sys_IsMainThread()");
    if (!p)
        return 0;
    if (!*p)
        return staticNULL;
    hash = hashForString(p);
    for (str = g_strHandle[hash]; str; str = str->next)
    {
        if (!str->str)
            MyAssertHandler(".\\ui\\ui_utils.cpp", 475, 0, "%s", "str->str");
        if (!strcmp(p, str->str))
            return str->str;
    }
    s = UI_Alloc(strlen(p) + 1, 1);
    v4 = p;
    v3 = s;
    do
    {
        v2 = *v4;
        *v3++ = *v4++;
    } while (v2);
    stra = g_strHandle[hash];
    last = stra;
    while (stra && stra->next)
    {
        last = stra;
        stra = stra->next;
    }
    strb = (stringDef_s *)UI_Alloc(sizeof(stringDef_s), alignof(stringDef_s));
    strb->next = 0;
    strb->str = (const char *)s;
    if (last)
        last->next = strb;
    else
        g_strHandle[hash] = strb;
    return (const char *)s;
}

int __cdecl hashForString(const char *str)
{
    int hash; // [esp+0h] [ebp-Ch]
    int i; // [esp+8h] [ebp-4h]

    hash = 0;
    for (i = 0; str[i]; ++i)
        hash += tolower(str[i]) * (i + 119);
    return hash & 0x7FF;
}
