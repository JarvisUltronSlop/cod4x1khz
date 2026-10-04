#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include "com_memory.h"
#include <cstring>
#include <cstdlib>
#ifdef COD4X1KHZ
#include <khz/khz_controls.h>
#endif

// String ownership and enum table accesses use native pointers on both hosts.
// Numeric dvar values retain the existing 32-bit game representation.

const char *__cdecl Dvar_EnumToString(const dvar_s *dvar)
{
    if (!dvar)
        MyAssertHandler(".\\universal\\dvar.cpp", 278, 0, "%s", "dvar");
    if (!dvar->name)
        MyAssertHandler(".\\universal\\dvar.cpp", 279, 0, "%s", "dvar->name");
    if (dvar->type != DVAR_TYPE_ENUM)
        MyAssertHandler(
            ".\\universal\\dvar.cpp",
            280,
            0,
            "%s\n\t(dvar->name) = %s",
            "(dvar->type == DVAR_TYPE_ENUM)",
            dvar->name);
    if (!dvar->domain.enumeration.strings)
        MyAssertHandler(
            ".\\universal\\dvar.cpp",
            281,
            0,
            "%s\n\t(dvar->name) = %s",
            "(dvar->domain.enumeration.strings)",
            dvar->name);
    if ((dvar->current.integer < 0 || dvar->current.integer >= dvar->domain.enumeration.stringCount)
        && dvar->current.integer)
    {
        MyAssertHandler(
            ".\\universal\\dvar.cpp",
            282,
            0,
            "%s\n\t(dvar->current.integer) = %i",
            "(dvar->current.integer >= 0 && dvar->current.integer < dvar->domain.enumeration.stringCount || dvar->current.integer == 0)",
            dvar->current.integer);
    }
    if (dvar->domain.enumeration.stringCount)
        return dvar->domain.enumeration.strings[dvar->current.integer];
    else
        return "";
}

const char *__cdecl Dvar_IndexStringToEnumString(const dvar_s *dvar, const char *indexString)
{
    signed int v3; // [esp+0h] [ebp-1Ch]
    int enumIndex; // [esp+14h] [ebp-8h]
    int indexStringIndex; // [esp+18h] [ebp-4h]

    if (!dvar)
        MyAssertHandler(".\\universal\\dvar.cpp", 296, 0, "%s", "dvar");
    if (!dvar->name)
        MyAssertHandler(".\\universal\\dvar.cpp", 297, 0, "%s", "dvar->name");
    if (dvar->type != DVAR_TYPE_ENUM)
        MyAssertHandler(
            ".\\universal\\dvar.cpp",
            298,
            0,
            "%s\n\t(dvar->name) = %s",
            "(dvar->type == DVAR_TYPE_ENUM)",
            dvar->name);
    if (!dvar->domain.enumeration.strings)
        MyAssertHandler(
            ".\\universal\\dvar.cpp",
            299,
            0,
            "%s\n\t(dvar->name) = %s",
            "(dvar->domain.enumeration.strings)",
            dvar->name);
    if (!indexString)
        MyAssertHandler(".\\universal\\dvar.cpp", 300, 0, "%s\n\t(dvar->name) = %s", "(indexString)", dvar->name);
    if (!dvar->domain.enumeration.stringCount)
        return "";
    v3 = strlen(indexString);
    for (indexStringIndex = 0; indexStringIndex < v3; ++indexStringIndex)
    {
        if (!isdigit(indexString[indexStringIndex]))
            return "";
    }
    enumIndex = atoi(indexString);
    if (enumIndex >= 0 && enumIndex < dvar->domain.enumeration.stringCount)
        return dvar->domain.enumeration.strings[enumIndex];
    else
        return "";
}

int __cdecl Dvar_StringToEnum(const DvarLimits *domain, const char *string)
{
    int v3; // [esp+0h] [ebp-1Ch]
    int stringIndex; // [esp+14h] [ebp-8h]
    int stringIndexa; // [esp+14h] [ebp-8h]
    int stringIndexb; // [esp+14h] [ebp-8h]
    const char *digit; // [esp+18h] [ebp-4h]

    if (!domain)
        MyAssertHandler(".\\universal\\dvar.cpp", 420, 0, "%s", "domain");
    if (!string)
        MyAssertHandler(".\\universal\\dvar.cpp", 421, 0, "%s", "string");
    for (stringIndex = 0; stringIndex < domain->enumeration.stringCount; ++stringIndex)
    {
        if (!I_stricmp(string, domain->enumeration.strings[stringIndex]))
            return stringIndex;
    }
    stringIndexa = 0;
    for (digit = string; *digit; ++digit)
    {
        if (*digit < 48 || *digit > 57)
            return -1337;
        stringIndexa = 10 * stringIndexa + *digit - 48;
    }
    if (stringIndexa >= 0 && stringIndexa < domain->enumeration.stringCount)
        return stringIndexa;
    v3 = strlen(string);
    for (stringIndexb = 0; stringIndexb < domain->enumeration.stringCount; ++stringIndexb)
    {
        if (!I_strnicmp(string, domain->enumeration.strings[stringIndexb], v3))
            return stringIndexb;
    }
    return -1337;
}

bool __cdecl Dvar_ShouldFreeCurrentString(dvar_s *dvar)
{
    return dvar->current.string
        && dvar->current.string != dvar->latched.string
        && dvar->current.string != dvar->reset.string;
}

bool __cdecl Dvar_ShouldFreeLatchedString(dvar_s *dvar)
{
    return dvar->latched.string
        && dvar->latched.string != dvar->current.string
        && dvar->latched.string != dvar->reset.string;
}

bool __cdecl Dvar_ShouldFreeResetString(dvar_s *dvar)
{
    return dvar->reset.string
        && dvar->reset.string != dvar->current.string
        && dvar->reset.string != dvar->latched.string;
}

void __cdecl Dvar_FreeString(DvarValue *value)
{
    FreeString(value->string);
    value->string = 0;
}

void __cdecl Dvar_CopyString(const char *string, DvarValue *value)
{
    if (!string)
        MyAssertHandler(".\\universal\\dvar.cpp", 203, 0, "%s", "string");
    value->string = CopyString(string);
}

void __cdecl Dvar_WeakCopyString(const char *string, DvarValue *value)
{
    if (!string)
        MyAssertHandler(".\\universal\\dvar.cpp", 210, 0, "%s", "string");
    value->string = string;
}

void __cdecl Dvar_AssignResetStringValue(dvar_s *dvar, DvarValue *dest, const char *string)
{
    if (!string)
        MyAssertHandler(".\\universal\\dvar.cpp", 266, 0, "%s", "string");
    if (dvar->current.string && (string == (char *)dvar->current.string || !strcmp(string, dvar->current.string)))
    {
        Dvar_WeakCopyString(dvar->current.string, dest);
    }
    else if (dvar->latched.string && (string == (char *)dvar->latched.string || !strcmp(string, dvar->latched.string)))
    {
        Dvar_WeakCopyString(dvar->latched.string, dest);
    }
    else
    {
        Dvar_CopyString(string, dest);
    }
}

void __cdecl Dvar_AssignCurrentStringValue(dvar_s *dvar, DvarValue *dest, char *string)
{
    if (!string)
        MyAssertHandler(".\\universal\\dvar.cpp", 242, 0, "%s", "string");
    if (dvar->latched.string && (string == (char *)dvar->latched.string || !strcmp(string, dvar->latched.string)))
    {
        Dvar_WeakCopyString(dvar->latched.string, dest);
    }
    else if (dvar->reset.string && (string == (char *)dvar->reset.string || !strcmp(string, dvar->reset.string)))
    {
        Dvar_WeakCopyString(dvar->reset.string, dest);
    }
    else
    {
        Dvar_CopyString(string, dest);
    }
}

void __cdecl Dvar_AssignLatchedStringValue(dvar_s *dvar, DvarValue *dest, char *string)
{
    if (!string)
        MyAssertHandler(".\\universal\\dvar.cpp", 254, 0, "%s", "string");
    if (dvar->current.string && (string == (char *)dvar->current.string || !strcmp(string, dvar->current.string)))
    {
        Dvar_WeakCopyString(dvar->current.string, dest);
    }
    else if (dvar->reset.string && (string == (char *)dvar->reset.string || !strcmp(string, dvar->reset.string)))
    {
        Dvar_WeakCopyString(dvar->reset.string, dest);
    }
    else
    {
        Dvar_CopyString(string, dest);
    }
}

void __cdecl Dvar_UpdateResetValue(dvar_s *dvar, DvarValue value)
{
    DvarValue oldString; // [esp+10h] [ebp-28h] BYREF
    bool shouldFree; // [esp+23h] [ebp-15h]
    DvarValue resetString; // [esp+24h] [ebp-14h] BYREF

    iassert(dvar);

    switch (dvar->type)
    {
    case DVAR_TYPE_FLOAT_2:
        dvar->reset.value = value.value;
        dvar->reset.vector[1] = value.vector[1];
        break;
    case DVAR_TYPE_FLOAT_3:
        dvar->reset.value = value.value;
        dvar->reset.vector[1] = value.vector[1];
        dvar->reset.vector[2] = value.vector[2];
        break;
    case DVAR_TYPE_FLOAT_4:
        dvar->reset = value;
        break;
    case DVAR_TYPE_STRING:
        if (dvar->reset.string != value.string)
        {
            shouldFree = Dvar_ShouldFreeResetString(dvar);
            if (shouldFree)
                oldString.string = dvar->reset.string;
            Dvar_AssignResetStringValue(dvar, &resetString, (char *)value.string);
            dvar->reset.string = resetString.string;
            if (shouldFree)
                Dvar_FreeString(&oldString);
        }
        break;
    default:
        dvar->reset = value;
        break;
    }
}

void __cdecl Dvar_UpdateValue(dvar_s *dvar, DvarValue value)
{
    DvarValue oldString; // [esp+1Ch] [ebp-28h] BYREF
    bool shouldFree; // [esp+2Fh] [ebp-15h]
    DvarValue currentString; // [esp+30h] [ebp-14h] BYREF

    if (!dvar)
        MyAssertHandler(".\\universal\\dvar.cpp", 1393, 0, "%s", "dvar");
    switch (dvar->type)
    {
    case 2u:
        dvar->current.value = value.value;
        dvar->current.vector[1] = value.vector[1];
        dvar->latched.value = value.value;
        dvar->latched.vector[1] = value.vector[1];
        break;
    case 3u:
        dvar->current.value = value.value;
        dvar->current.vector[1] = value.vector[1];
        dvar->current.vector[2] = value.vector[2];
        dvar->latched.value = value.value;
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        break;
    case 4u:
        dvar->current = value;
        dvar->latched = value;
        break;
    case 7u:
        if (value.string != dvar->current.string)
        {
            shouldFree = Dvar_ShouldFreeCurrentString(dvar);
            if (shouldFree)
                oldString.string = dvar->current.string;
            Dvar_AssignCurrentStringValue(dvar, &currentString, (char *)value.string);
            dvar->current.string = currentString.string;
            if (Dvar_ShouldFreeLatchedString(dvar))
                Dvar_FreeString(&dvar->latched);
            dvar->latched.string = 0;
            Dvar_WeakCopyString(dvar->current.string, &dvar->latched);
            if (shouldFree)
                Dvar_FreeString(&oldString);
        }
        break;
    default:
        dvar->current = value;
        dvar->latched = value;
        break;
    }
}

void __cdecl Dvar_SetLatchedValue(dvar_s *dvar, DvarValue value)
{
#ifdef COD4X1KHZ
    KHZ_DvarScope synchronization;
#endif
    DvarValue latchedString; // [esp+10h] [ebp-28h] BYREF
    DvarValue oldString; // [esp+20h] [ebp-18h] BYREF
    bool shouldFree; // [esp+33h] [ebp-5h]

    switch (dvar->type)
    {
    case 2u:
        dvar->latched.value = value.value;
        dvar->latched.vector[1] = value.vector[1];
        break;
    case 3u:
        dvar->latched.value = value.value;
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        break;
    case 4u:
        dvar->latched = value;
        break;
    case 7u:
        if (dvar->latched.string != value.string)
        {
            shouldFree = Dvar_ShouldFreeLatchedString(dvar);
            if (shouldFree)
                oldString.string = dvar->latched.string;
            Dvar_AssignLatchedStringValue(dvar, &latchedString, (char*)value.string);
            dvar->latched.string = latchedString.string;
            if (shouldFree)
                Dvar_FreeString(&oldString);
        }
        break;
    default:
        dvar->latched = value;
        break;
    }
}
