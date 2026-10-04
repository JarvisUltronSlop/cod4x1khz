#include <universal/q_shared.h>
#include "ui_shared.h"
#include "ui_memory.h"
#include <universal/com_memory.h>

void __cdecl free_expression(statement_s *statement)
{
    expressionEntry *entry; // [esp+0h] [ebp-8h]
    int entryNum; // [esp+4h] [ebp-4h]

    if (statement->numEntries)
    {
        for (entryNum = 0; entryNum < statement->numEntries; ++entryNum)
        {
            entry = statement->entries[entryNum];
            if (entry->type == 1 && entry->data.operand.dataType == VAL_STRING)
                Z_Free((char *)entry->data.operand.internals.string, 34);
            Z_Free((char *)entry, 34);
            statement->entries[entryNum] = 0;
        }
        Z_Free((char *)statement->entries, 34);
        statement->entries = 0;
        statement->numEntries = 0;
    }
}

void __cdecl Statement_AddEntry(statement_s *statement, expressionEntry *entry)
{
    statement->entries[statement->numEntries++] = entry;
}

void __cdecl Statement_AddOperator(statement_s *statement, operationEnum op)
{
    auto *entry = static_cast<expressionEntry *>(Z_Malloc(sizeof(expressionEntry), "Statement_AddOperator", 34));
    entry->type = 0;
    entry->data.op = op;
    Statement_AddEntry(statement, entry);
}

void __cdecl Statement_AddIntOperand(statement_s *statement, int val)
{
    auto *entry = static_cast<expressionEntry *>(Z_Malloc(sizeof(expressionEntry), "Statement_AddIntOperand", 34));
    entry->type = 1;
    entry->data.operand.dataType = VAL_INT;
    entry->data.operand.internals.intVal = val;
    Statement_AddEntry(statement, entry);
}

void __cdecl Statement_AddFloatOperand(statement_s *statement, float val)
{
    auto *entry = static_cast<expressionEntry *>(Z_Malloc(sizeof(expressionEntry), "Statement_AddFloatOperand", 34));
    entry->type = 1;
    entry->data.operand.dataType = VAL_FLOAT;
    entry->data.operand.internals.floatVal = val;
    Statement_AddEntry(statement, entry);
}

void __cdecl Statement_AddStringOperand(statement_s *statement, char *str)
{
    expressionEntry *entry; // [esp+20h] [ebp-4h]

    entry = (expressionEntry *)Z_Malloc(sizeof(expressionEntry), "Statement_AddStringOperand", 34);
    entry->type = 1;
    entry->data.operand.dataType = VAL_STRING;
    entry->data.operand.internals.string = (char *)Z_Malloc(strlen(str) + 1, "Statement_AddStringOperand", 34);
    I_strncpyz((char *)entry->data.operand.internals.string, str, strlen(str) + 1);
    Statement_AddEntry(statement, entry);
}

operandInternalDataUnion __cdecl GetSourceInt(Operand *source)
{
    if (source->dataType == VAL_FLOAT)
    {
        return (operandInternalDataUnion)(int)source->internals.floatVal;
    }
    else if (source->dataType)
    {
        if (source->dataType != VAL_STRING)
            MyAssertHandler(".\\ui\\ui_expressions.cpp", 222, 0, "%s", "source->dataType == VAL_STRING");
        return (operandInternalDataUnion)atoi(source->internals.string);
    }
    else
    {
        return source->internals;
    }
}

double __cdecl GetSourceFloat(Operand *source)
{
    if (source->dataType == VAL_FLOAT)
        return source->internals.floatVal;
    if (source->dataType == VAL_INT)
        return (double)source->internals.intVal;
    if (source->dataType != VAL_STRING)
        MyAssertHandler(".\\ui\\ui_expressions.cpp", 211, 0, "%s", "source->dataType == VAL_STRING");
    return (float)atof(source->internals.string);
}
