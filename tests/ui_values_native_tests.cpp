#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <ui/ui_memory.h>
#include <bgame/bg_local.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
static std::unordered_map<void *, size_t> allocations;
static std::unordered_set<const char *> strings;
void MyAssertHandler(const char *, int, int, const char *, ...) { throw std::runtime_error("UI assertion"); }
void Com_PrintError(int, const char *, ...) {}
void Com_Error(errorParm_t, const char *, ...) { throw std::runtime_error("UI fatal error"); }
int I_stricmp(const char *a, const char *b) { return _stricmp(a, b); }
void I_strncpyz(char *out, const char *in, int count) { CHECK(count > 0); strncpy_s(out, count, in, _TRUNCATE); }
int Com_sprintf(char *out, uint32_t size, const char *format, ...) {
    va_list args; va_start(args, format); int n = vsnprintf(out, size, format, args); va_end(args); return n;
}
void *Z_Malloc(int size, const char *, int) {
    CHECK(size > 0);
    void *p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE); CHECK(p);
    if constexpr (sizeof(void *) == 8) CHECK(reinterpret_cast<uintptr_t>(p) > UINT32_MAX);
    allocations.emplace(p, size); memset(p, 0xcd, size); return p;
}
void Z_Free(void *p, int) { CHECK(allocations.erase(p) == 1); CHECK(VirtualFree(p, 0, MEM_RELEASE)); }
const char *CopyString(const char *value) {
    auto *p = static_cast<char *>(Z_Malloc(static_cast<int>(strlen(value) + 1), "fixture", 0));
    strcpy(p, value); strings.insert(p); return p;
}
void FreeString(const char *value) { CHECK(strings.erase(value) == 1); Z_Free(const_cast<char *>(value), 0); }

bool Sys_IsMainThread() { return true; }
static std::unordered_map<void *, size_t> uiAllocations;
uint8_t *UI_Alloc(uint32_t size, int alignment) {
    CHECK(alignment > 0 && alignment <= 16);
    auto *base = static_cast<uint8_t *>(Z_Malloc(static_cast<int>(size + 32), "UI fixture", 34));
    auto *value = base + 16; CHECK(reinterpret_cast<uintptr_t>(value) % alignment == 0);
    uiAllocations.emplace(value, size); return value;
}
static void checkUiCanaries() {
    for (const auto &entry : uiAllocations) {
        auto *value = static_cast<uint8_t *>(entry.first);
        for (unsigned i = 0; i < 16; ++i) CHECK(value[-16 + static_cast<int>(i)] == 0xcd && value[entry.second + i] == 0xcd);
    }
}
int main() {
    String_Init(); CHECK(!String_Alloc(nullptr) && !strcmp(String_Alloc(""), ""));
    const char *interned = String_Alloc("All"); CHECK(!strcmp(interned, "All"));
    if constexpr (sizeof(void *) == 8) CHECK(reinterpret_cast<uintptr_t>(interned) > UINT32_MAX);
    const auto firstAllocations = uiAllocations.size();
    CHECK(String_Alloc("All") == interned && uiAllocations.size() == firstAllocations);
    char collision[32]{};
    for (unsigned n = 0; n < 100000; ++n) {
        snprintf(collision, sizeof(collision), "collision_%u", n);
        if (hashForString(collision) == hashForString("All")) break;
    }
    CHECK(hashForString(collision) == hashForString("All"));
    const char *otherString = String_Alloc(collision);
    CHECK(!strcmp(otherString, collision) && String_Alloc("All") == interned && String_Alloc(collision) == otherString);
    checkUiCanaries();
    String_Init(); CHECK(String_Alloc("All") != interned); checkUiCanaries();
    for (const auto &entry : uiAllocations) Z_Free(static_cast<uint8_t *>(entry.first) - 16, 34);
    uiAllocations.clear(); String_Init();
    char *high = static_cast<char *>(Z_Malloc(4096, "high input", 0)); strcpy(high, "42");
    Operand first{VAL_STRING, operandInternalDataUnion(high)}, other{VAL_STRING, operandInternalDataUnion("x")}, result{};
    CHECK(GetSourceInt(&first).intVal == 42 && GetSourceFloat(&first) == 42.0);
    add_StringWithString(&first, &other, &result); CHECK(result.dataType == VAL_STRING && !strcmp(result.internals.string, "42x"));
    OperandStack operands{};
    AddOperandToStack(&operands, &first); CHECK(operands.stack[0].operands[0].internals.string == high);
    CHECK(GetOperand(&operands, &result)); CHECK(result.dataType == VAL_STRING && result.internals.string == high);
    AddOperandToStack(&operands, &first); AddOperandToStack(&operands, &other);
    OperandList left{}, right{}; CHECK(GetOperandList(&operands, &right)); CHECK(GetOperandList(&operands, &left));
    RunCommaOp(0, &operands, &left, &right); CHECK(GetOperandList(&operands, &left));
    CHECK(left.operandCount == 2 && left.operands[0].internals.string == high && left.operands[1].internals.string == other.internals.string);
    Operand number{VAL_INT, operandInternalDataUnion(7)};
    add_StringWithInt(&first, &number, &result); CHECK(!strcmp(result.internals.string, "427"));
    add_IntWithString(&number, &first, &result); CHECK(!strcmp(result.internals.string, "742"));
    Operand decimal{VAL_FLOAT, operandInternalDataUnion(1.5f)};
    add_StringWithFloat(&first, &decimal, &result); CHECK(!strcmp(result.internals.string, "421.500000"));
    add_FloatWithString(&decimal, &first, &result); CHECK(!strcmp(result.internals.string, "1.50000042"));
    and_StringWithInt(&first, &number, &result); CHECK(result.internals.intVal == 1);
    high[0] = 0; and_StringWithInt(&first, &number, &result); CHECK(result.internals.intVal == 0);
    or_StringWithFloat(&first, &decimal, &result); CHECK(result.internals.intVal == 1); strcpy(high, "42");
    other.internals.string = high; compare_doesStringEqualString(&first, &other, &result); CHECK(result.internals.intVal == 1);

    UILocalVarContext context{}; char name[] = "native_string";
    auto *var = reinterpret_cast<UILocalVar *>(UILocalVar_FindOrCreate(&context, name));
    CHECK(var == &context.table[UILocalVar_HashName(name)]);
    CHECK(reinterpret_cast<UILocalVar *>(UILocalVar_Find(&context, name)) == var);
    CHECK(reinterpret_cast<UILocalVar *>(UILocalVar_FindOrCreate(&context, name)) == var);
    UILocalVar_SetString(var, high); char buffer[64];
    CHECK(UILocalVar_GetString(var, buffer, sizeof(buffer)) == var->u.string);
    CHECK(UILocalVar_GetInt(var).integer == 42 && UILocalVar_GetBool(var));
    UILocalVar_SetFloat(var, 2.5f); CHECK(strings.size() == 1 && UILocalVar_GetFloat(var) == 2.5);
    UILocalVar_SetString(var, high); UILocalVar_SetInt(var, -19); CHECK(strings.size() == 1 && UILocalVar_GetInt(var).integer == -19);
    UILocalVar_Shutdown(&context); CHECK(strings.empty());

    token_s token{}, second{}; strcpy(token.string, "macro"); token.line = 17; token.whitespace_p = high;
    token.endwhitespace_p = high + 2; token.next = &second; second.line = 23; strcpy(second.string, "value");
    define_s definition{}; definition.name = name; definition.flags = 3; definition.builtin = 4;
    definition.numparms = 2; definition.tokens = &token; definition.parms = &second;
    define_s *copy = PC_CopyDefine(nullptr, &definition);
    CHECK(!strcmp(copy->name, name) && copy->flags == 3 && copy->builtin == 4 && copy->numparms == 2);
    CHECK(copy->tokens != &token && copy->tokens->next && copy->tokens->next->line == 23);
    CHECK(copy->tokens->whitespace_p == high && copy->tokens->endwhitespace_p == high + 2);
    CHECK(!copy->tokens->next->next && copy->parms != &second && !copy->parms->next && numtokens == 3);
    CHECK(reinterpret_cast<uintptr_t>(copy->tokens) % alignof(token_s) == 0);
    PC_FreeToken(copy->tokens->next); PC_FreeToken(copy->tokens); PC_FreeToken(copy->parms);
    FreeMemory(reinterpret_cast<char *>(copy)); CHECK(numtokens == 0);

    statement_s statement{};
    statement.entries = static_cast<expressionEntry **>(Z_Malloc(4 * sizeof(expressionEntry *), "entries", 34));
    Statement_AddOperator(&statement, OP_ADD); Statement_AddIntOperand(&statement, -81);
    Statement_AddFloatOperand(&statement, 3.25f); Statement_AddStringOperand(&statement, high);
    CHECK(statement.numEntries == 4 && statement.entries[0]->data.op == OP_ADD);
    CHECK(statement.entries[1]->data.operand.internals.intVal == -81);
    CHECK(statement.entries[2]->data.operand.internals.floatVal == 3.25f);
    CHECK(statement.entries[3]->data.operand.dataType == VAL_STRING && !strcmp(statement.entries[3]->data.operand.internals.string, high));
    for (int i = 0; i < 4; ++i) CHECK(allocations.at(statement.entries[i]) == sizeof(expressionEntry));
    free_expression(&statement); CHECK(!statement.entries && !statement.numEntries);

    auto *huds = static_cast<hudelem_s *>(Z_Malloc(2 * sizeof(hudelem_s), "HUDs", 0));
    huds[0].sort = 9.0f; huds[1].sort = -2.0f; const hudelem_s *order[]{&huds[0], &huds[1]};
    qsort(order, 2, sizeof(order[0]), compare_hudelems); CHECK(order[0] == &huds[1]);
    Z_Free(huds, 0); Z_Free(high, 0); CHECK(allocations.empty());
    std::printf("Production UI: %zu-bit expressions, strings, local variables, macro copying, allocation lifecycle and HUD sorting passed\n", sizeof(void *) * 8);
}
