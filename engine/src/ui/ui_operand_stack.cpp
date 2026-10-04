#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include "ui_shared.h"

void __cdecl AddOperandToStack(OperandStack *dataStack, Operand *data)
{
    operandInternalDataUnion v2; // ecx
    int numOperandLists; // edx

    if (data->dataType == VAL_STRING && !data->internals.string)
        MyAssertHandler(
            ".\\ui\\ui_expressions.cpp",
            1605,
            0,
            "%s",
            "data->dataType != VAL_STRING || data->internals.string");
    v2 = data->internals;
    numOperandLists = dataStack->numOperandLists;
    dataStack->stack[numOperandLists].operands[0].dataType = data->dataType;
    dataStack->stack[numOperandLists].operands[0].internals = v2;
    dataStack->stack[dataStack->numOperandLists++].operandCount = 1;
}

char __cdecl GetOperand(OperandStack *dataStack, Operand *data)
{
    operandInternalDataUnion v2; // edx
    operandInternalDataUnion v4; // eax
    OperandList *list; // [esp+0h] [ebp-4h]

    if (dataStack->numOperandLists >= 1)
    {
        list = &dataStack->stack[dataStack->numOperandLists - 1];
        if (list->operandCount == 1)
        {
            v4 = list->operands[0].internals;
            data->dataType = list->operands[0].dataType;
            data->internals = v4;
            --dataStack->numOperandLists;
            if (data->dataType == VAL_STRING && !data->internals.string)
                MyAssertHandler(
                    ".\\ui\\ui_expressions.cpp",
                    1796,
                    0,
                    "%s",
                    "data->dataType != VAL_STRING || data->internals.string");
            return 1;
        }
        else
        {
            Com_PrintError(
                CON_CHANNEL_UI,
                "Error: Invalid operand count - expected to find one operand but instead found %i\n",
                list->operandCount);
            data->dataType = VAL_INT;
            data->internals.intVal = 0;
            return 0;
        }
    }
    else
    {
        Com_PrintError(CON_CHANNEL_UI, "Error: Invalid operation - missing parameter inside function or parenthesis\n");
        dataStack->numOperandLists = 1;
        dataStack->stack[0].operandCount = 1;
        v2 = dataStack->stack[0].operands[0].internals;
        data->dataType = dataStack->stack[0].operands[0].dataType;
        data->internals = v2;
        data->dataType = VAL_INT;
        data->internals.intVal = 0;
        return 0;
    }
}

char __cdecl GetOperandList(OperandStack *dataStack, OperandList *list)
{
    if (dataStack->numOperandLists >= 1)
    {
        memcpy(list, &dataStack->stack[--dataStack->numOperandLists], sizeof(OperandList));
        return 1;
    }
    else
    {
        Com_PrintError(CON_CHANNEL_UI, "Error: Invalid operation - missing parameter inside function or parenthesis\n");
        dataStack->numOperandLists = 1;
        memcpy(list, dataStack, sizeof(OperandList));
        list->operandCount = 1;
        list->operands[0].dataType = VAL_INT;
        list->operands[0].internals.intVal = 0;
        return 0;
    }
}

void __cdecl RunCommaOp(int localClientNum, OperandStack *dataStack, OperandList *list1, OperandList *list2)
{
    operandInternalDataUnion v4; // edx
    operandInternalDataUnion v5; // eax
    int list1Operand; // [esp+4h] [ebp-18h]
    Operand *finalList; // [esp+8h] [ebp-14h]
    int operand; // [esp+Ch] [ebp-10h]
    Operand operandResult; // [esp+10h] [ebp-Ch] BYREF
    int list2Operand; // [esp+18h] [ebp-4h]

    if (list2->operandCount + list1->operandCount <= 10)
    {
        dataStack->stack[dataStack->numOperandLists].operandCount = list2->operandCount + list1->operandCount;
        finalList = dataStack->stack[dataStack->numOperandLists].operands;
        operand = 0;
        for (list1Operand = 0; list1Operand < list1->operandCount; ++list1Operand)
        {
            v4 = list1->operands[list1Operand].internals;
            finalList[operand].dataType = list1->operands[list1Operand].dataType;
            finalList[operand].internals = v4;
            if (finalList[operand].dataType == VAL_STRING && !finalList[operand].internals.string)
                MyAssertHandler(
                    ".\\ui\\ui_expressions.cpp",
                    1637,
                    0,
                    "%s",
                    "finalList[operand].dataType != VAL_STRING || finalList[operand].internals.string");
            ++operand;
        }
        for (list2Operand = 0; list2Operand < list2->operandCount; ++list2Operand)
        {
            v5 = list2->operands[list2Operand].internals;
            finalList[operand].dataType = list2->operands[list2Operand].dataType;
            finalList[operand].internals = v5;
            if (finalList[operand].dataType == VAL_STRING && !finalList[operand].internals.string)
                MyAssertHandler(
                    ".\\ui\\ui_expressions.cpp",
                    1643,
                    0,
                    "%s",
                    "finalList[operand].dataType != VAL_STRING || finalList[operand].internals.string");
            ++operand;
        }
        ++dataStack->numOperandLists;
    }
    else
    {
        Com_PrintError(CON_CHANNEL_UI, "UI Script error: No function takes %i parameters\n", list2->operandCount + list1->operandCount);
        operandResult.dataType = VAL_INT;
        operandResult.internals.intVal = 0;
        AddOperandToStack(dataStack, &operandResult);
    }
}
