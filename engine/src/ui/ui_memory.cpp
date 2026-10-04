#include <universal/q_shared.h>
#include "ui_memory.h"
#include <universal/com_memory.h>

int numtokens;

uint32_t *__cdecl GetMemory(uint32_t size)
{
    uint32_t *ptr; // [esp+4h] [ebp-4h]

    constexpr size_t prefixBytes = alignof(token_s);
    ptr = (uint32_t *)Z_Malloc(size + prefixBytes, "GetMemory", 10);
    if (!ptr)
        return 0;
    *ptr = 0x12345678;
    return reinterpret_cast<uint32_t *>(reinterpret_cast<char *>(ptr) + prefixBytes);
}

void __cdecl FreeMemory(char *ptr)
{
    char *base = ptr - alignof(token_s);
    if (*reinterpret_cast<uint32_t *>(base) == 0x12345678)
        Z_Free(base, 10);
}

uint8_t *__cdecl GetClearedMemory(uint32_t size)
{
    uint8_t *ptr; // [esp+0h] [ebp-4h]

    ptr = (uint8_t *)GetMemory(size);
    memset(ptr, 0, size);
    return ptr;
}

token_s *__cdecl PC_CopyToken(token_s *token)
{
    uint32_t *t; // [esp+8h] [ebp-4h]

    t = GetMemory(sizeof(token_s));
    if (t)
    {
        memcpy(t, token, sizeof(token_s));
        reinterpret_cast<token_s *>(t)->next = nullptr;
        ++numtokens;
        return (token_s *)t;
    }
    else
    {
        Com_Error(ERR_FATAL, "EXE_ERR_OUT_OF_MEMORY");
        return 0;
    }
}

void __cdecl PC_FreeToken(token_s *token)
{
    FreeMemory(token->string);
    --numtokens;
}

define_s *__cdecl PC_CopyDefine(source_s *source, define_s *define)
{
    const size_t nameBytes = strlen(define->name) + 1;
    auto *copy = reinterpret_cast<define_s *>(GetMemory(sizeof(define_s) + nameBytes));
    memset(copy, 0, sizeof(*copy));
    copy->name = reinterpret_cast<char *>(copy + 1);
    memcpy(copy->name, define->name, nameBytes);
    copy->flags = define->flags;
    copy->builtin = define->builtin;
    copy->numparms = define->numparms;
    token_s **tail = &copy->tokens;
    for (token_s *token = define->tokens; token; token = token->next)
    {
        *tail = PC_CopyToken(token);
        (*tail)->next = nullptr;
        tail = &(*tail)->next;
    }
    tail = &copy->parms;
    for (token_s *token = define->parms; token; token = token->next)
    {
        *tail = PC_CopyToken(token);
        (*tail)->next = nullptr;
        tail = &(*tail)->next;
    }
    return copy;
}
