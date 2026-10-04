#include <Windows.h>
#include <script/scr_stack_archive.h>
#include <script/scr_debugger.h>
#include <script/scr_compiler.h>
#include <script/scr_bytecode.h>
#include <script/scr_parsetree.h>
#include <script/scr_animtree.h>
#include <qcommon/qcommon.h>
#include <universal/com_memory.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
scrCompileGlob_t scrCompileGlob{};
scrCompilePub_t scrCompilePub{};
static char *emitStart;
static uint32_t emitBytes;
char *TempMalloc(uint32_t bytes) { CHECK(emitBytes + bytes < 4096); auto *p = emitStart + emitBytes; emitBytes += bytes; return p; }
char *TempMallocAlignStrict(uint32_t bytes) { return TempMalloc(bytes); }

static char* nodeMemory;
static uint32_t nodeBytes;
static std::array<char*, 128> nodeGuards{};
static unsigned nodeGuardCount;
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
HunkUser* Hunk_UserCreate(int, const char*, bool, bool, int) { std::abort(); }
void Hunk_UserDestroy(HunkUser*) { std::abort(); }
void* Hunk_UserAlloc(HunkUser* user, uint32_t bytes, int alignment) {
    CHECK(user == g_allocNodeUser && alignment == alignof(sval_u));
    nodeBytes = (nodeBytes + alignment - 1) & ~(alignment - 1);
    CHECK(nodeBytes + bytes + 16 < 32768 && nodeGuardCount < nodeGuards.size());
    auto* result = nodeMemory + nodeBytes;
    auto* guard = result + bytes; std::memset(guard, 0xad, 16);
    nodeGuards[nodeGuardCount++] = guard; nodeBytes += bytes + 16; return result;
}
static void checkNodeGuards() {
    for (unsigned guard = 0; guard < nodeGuardCount; ++guard)
        for (unsigned byte = 0; byte < 16; ++byte)
            CHECK(static_cast<unsigned char>(nodeGuards[guard][byte]) == 0xad);
}

static std::array<VariableValueInternal_u, 4> animationVariables{};
static std::array<uint32_t, 4> animationNames{};
static uint32_t animationCount;
uint32_t FindVariable(uint32_t parent, uint32_t name) {
    CHECK(parent == 17);
    for (uint32_t i = 1; i <= animationCount; ++i) if (animationNames[i] == name) return i;
    return 0;
}
uint32_t GetNewVariable(uint32_t parent, uint32_t name) {
    CHECK(parent == 17 && animationCount + 1 < animationVariables.size());
    animationNames[++animationCount] = name; return animationCount;
}
VariableValueInternal_u* GetVariableValueAddress(uint32_t id) {
    CHECK(id && id <= animationCount); return &animationVariables[id];
}
void SetVariableValue(uint32_t id, VariableValue* value) {
    CHECK(value->type == VAR_CODEPOS); GetVariableValueAddress(id)->u = value->u;
}
const char* SL_ConvertToString(uint32_t) { return "fixture"; }
void Com_Error(errorParm_t, const char*, ...) { std::abort(); }

int main() {
    auto* memory = static_cast<char*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    CHECK(memory);
    if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(memory) > UINT32_MAX);
    std::array<VariableValue, 5> values{};
    values[0] = {VariableUnion(-123456), VAR_INTEGER};
    values[1] = {VariableUnion(0.375f), VAR_FLOAT};
    values[2] = {VariableUnion(memory + 512), VAR_CODEPOS};
    values[3].type = VAR_VECTOR; values[3].u.vectorValue = reinterpret_cast<float*>(memory + 1024);
    values[4].type = VAR_STACK; values[4].u.stackValue = reinterpret_cast<VariableStackBuffer*>(memory + 2048);
    const auto bytes = SCR_STACK_HEADER_BYTES + values.size() * SCR_STACK_ENTRY_BYTES;
    auto* stack = reinterpret_cast<VariableStackBuffer*>(memory);
    stack->size = static_cast<uint16_t>(values.size()); stack->bufLen = static_cast<uint16_t>(bytes);
    std::memset(memory + bytes, 0xac, 32);
    auto* cursor = stack->buf;
    for (const auto& value : values) { *cursor++ = static_cast<char>(value.type); Scr_WriteStackValue(cursor, value.u); cursor += SCR_STACK_VALUE_BYTES; }
    CHECK(cursor == memory + bytes);
    for (int i = 0; i < 32; ++i) CHECK(static_cast<unsigned char>(cursor[i]) == 0xac);
    for (size_t i = values.size(); i--;) {
        cursor -= SCR_STACK_VALUE_BYTES;
        const auto value = Scr_ReadStackValue(cursor); --cursor;
        CHECK(*cursor == values[i].type && std::memcmp(&value, &values[i].u, sizeof(value)) == 0);
    }
    CHECK(cursor == stack->buf);
    if constexpr (sizeof(void*) == 4) {
        const unsigned char scalarGolden[]{6, 0xc0, 0x1d, 0xfe, 0xff, 5, 0, 0, 0xc0, 0x3e};
        CHECK(std::memcmp(stack->buf, scalarGolden, sizeof(scalarGolden)) == 0);
    }
    sval_u original; original.node = reinterpret_cast<sval_u*>(memory + 4096);
    sval_u copy; copy = original; CHECK(copy.node == original.node);
    const sval_u fixed = original; copy = fixed; CHECK(copy.node == original.node);
    CHECK(sizeof(VariableUnion::intValue) == 4 && sizeof(VariableUnion::pointerValue) == 4);
    VariableUnion function;
    function.nativeFunctionValue = reinterpret_cast<intptr_t>(memory + 5000);
    CHECK(reinterpret_cast<char *>(function.nativeFunctionValue) == memory + 5000);
    // Real operand emitters preserve scalar widths in a deliberately unaligned stream.
    emitStart = memory + 8192; emitBytes = 0;
    EmitByte(0xef); EmitInteger(-123456); EmitFloat(0.375f); EmitString(321);
    EmitCodepos(memory + 4096); EmitInteger(7); EmitInteger(-1);
    const char *cursorCode = emitStart;
    CHECK(Scr_ReadOperand<uint8_t>(&cursorCode) == 0xef);
    CHECK(Scr_ReadOperand<int32_t>(&cursorCode) == -123456);
    CHECK(Scr_ReadOperand<float>(&cursorCode) == 0.375f);
    CHECK(Scr_ReadOperand<uint16_t>(&cursorCode) == 321);
    CHECK(Scr_ReadOperand<const char *>(&cursorCode) == memory + 4096);
    CHECK(Scr_ReadOperand<uint32_t>(&cursorCode) == 7);
    CHECK(Scr_ReadOperand<int32_t>(&cursorCode) == -1);
    CHECK(cursorCode == emitStart + emitBytes && emitBytes == 19 + sizeof(void *));
    emitBytes = 0;
    EmitInteger(0); EmitCodepos(memory + 64);
    EmitInteger(55); EmitCodepos(memory + 128);
    EmitInteger(3); EmitCodepos(memory + 192);
    CHECK(emitBytes == 3 * SCR_SWITCH_ENTRY_BYTES);
    std::qsort(emitStart, 3, SCR_SWITCH_ENTRY_BYTES, CompareCaseInfo);
    cursorCode = emitStart;
    CHECK(Scr_ReadOperand<uint32_t>(&cursorCode) == 55 && Scr_ReadOperand<const char *>(&cursorCode) == memory + 128);
    CHECK(Scr_ReadOperand<uint32_t>(&cursorCode) == 3 && Scr_ReadOperand<const char *>(&cursorCode) == memory + 192);
    CHECK(Scr_ReadOperand<uint32_t>(&cursorCode) == 0 && Scr_ReadOperand<const char *>(&cursorCode) == memory + 64);
    // Real break/continue patchers must preserve the next instruction.
    std::memset(memory + 12000, 0xac, 48);
    emitStart = memory + 12100; emitBytes = 0;
    BreakStatementInfo breakInfo{};
    breakInfo.codePos = memory + 12001; breakInfo.nextCodePos = memory + 12005;
    scrCompileGlob.currentBreakStatement = &breakInfo;
    ConnectBreakStatements();
    ContinueStatementInfo continueInfo{};
    continueInfo.codePos = memory + 12017; continueInfo.nextCodePos = memory + 12021;
    scrCompileGlob.currentContinueStatement = &continueInfo;
    ConnectContinueStatements();
    Scr_PatchJumpOffset(memory + 12033, -123);
    const char* patch = memory + 12001; CHECK(Scr_ReadOperand<int32_t>(&patch) == 95);
    patch = memory + 12017; CHECK(Scr_ReadOperand<int32_t>(&patch) == 79);
    patch = memory + 12033; CHECK(Scr_ReadOperand<int32_t>(&patch) == -123);
    for (int byte : {0, 5, 6, 7, 8, 16, 21, 22, 23, 24, 32, 37, 38, 39, 40})
        CHECK(static_cast<unsigned char>(memory[12000 + byte]) == 0xac);
    scrCompileGlob.currentBreakStatement = nullptr; scrCompileGlob.currentContinueStatement = nullptr;
    // Real parser constructors reserve native-sized union slots and preserve
    // the links of lists whose payloads and nodes live above 4 GiB.
    nodeMemory = memory + 16384; g_allocNodeUser = reinterpret_cast<HunkUser*>(memory + 60000);
    auto* tree = node8(ENUM_NOP, original, sval_u(2), original, sval_u(4), original,
        sval_u(6), original, sval_u(8)).node;
    checkNodeGuards(); CHECK(tree[1].node == original.node && tree[8].intValue == 8);
    auto program = node1(original, original);
    checkNodeGuards(); CHECK(program.node[0].node == original.node && program.node[1].node == original.node);
    auto list = linked_list_end(sval_u(17)); checkNodeGuards();
    list = append_node(list, sval_u(31)); checkNodeGuards();
    list = prepend_node(sval_u(5), list); checkNodeGuards();
    auto* at = list.node[0].node;
    for (int expected : {5, 17, 31}) { CHECK(at && at[0].intValue == expected); at = at[1].node; }
    CHECK(!at && list.node[1].node[0].intValue == 31);
    // Real animation fixups link full native addresses while writing only the
    // four-byte handle, including external handles next to live game data.
    std::memset(memory + 55000, 0xac, 32);
    auto* animA = memory + 55001; auto* animB = memory + 55009; auto* animC = memory + 55017;
    Scr_ClearAnimationFixups();
    Scr_EmitAnimationInternal(animA, 31, 17);
    Scr_EmitAnimationInternal(animB, 31, 17);
    Scr_EmitAnimationInternal(animC, 47, 17);
    CHECK(animationVariables[1].u.codePosValue == animB);
    ConnectScriptToAnim(17, 123, 91, 31, 5);
    ConnectScriptToAnim(17, 456, 91, 47, 6);
    scr_anim_s resolved{};
    std::memcpy(&resolved, animA, sizeof(resolved)); CHECK(resolved.index == 123 && resolved.tree == 5);
    std::memcpy(&resolved, animB, sizeof(resolved)); CHECK(resolved.index == 123 && resolved.tree == 5);
    std::memcpy(&resolved, animC, sizeof(resolved)); CHECK(resolved.index == 456 && resolved.tree == 6);
    CHECK(!animationVariables[1].u.codePosValue && !animationVariables[2].u.codePosValue);
    for (int byte : {0, 5, 6, 7, 8, 13, 14, 15, 16, 21, 22, 23})
        CHECK(static_cast<unsigned char>(memory[55000 + byte]) == 0xac);
    Scr_ClearAnimationFixups();
    g_allocNodeUser = nullptr;
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    std::printf("Script values: %zu-bit pointer payloads, unaligned archive round trips, canaries and parse-tree copies passed\n", sizeof(void*) * 8);
}
