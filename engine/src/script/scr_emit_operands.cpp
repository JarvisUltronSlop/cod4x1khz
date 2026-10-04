#include <universal/q_shared.h>
#include "scr_compiler.h"
#include "scr_bytecode.h"
#include <universal/com_memory.h>

void EmitCodepos(const char *pos)
{
	scrCompileGlob.codePos = (byte *)TempMallocAlignStrict(sizeof(const char *));
	memcpy(scrCompileGlob.codePos, &pos, sizeof(pos));
}

void EmitString(uint32_t value)
{
	scrCompileGlob.codePos = (byte *)TempMallocAlignStrict(sizeof(unsigned short));
	*(unsigned short *)scrCompileGlob.codePos = value;
}

void EmitFloat(float value)
{
	scrCompileGlob.codePos = (byte *)TempMallocAlignStrict(sizeof(float));
	*(float *)scrCompileGlob.codePos = value;
}

void EmitUnsignedShort(unsigned short value)
{
	scrCompileGlob.codePos = (byte *)TempMallocAlignStrict(sizeof(unsigned short));
	*(unsigned short *)scrCompileGlob.codePos = value;
}

void EmitShort(short value)
{
	scrCompileGlob.codePos = (byte *)TempMallocAlignStrict(sizeof(short));
	*(short *)scrCompileGlob.codePos = value;
}

void EmitInteger(int value)
{
	scrCompileGlob.codePos = (byte *)TempMallocAlignStrict(sizeof(int));
	*(int *)scrCompileGlob.codePos = value;
}

void EmitByte(byte value)
{
	scrCompileGlob.codePos = (byte *)TempMalloc(sizeof(byte));
	*(byte *)scrCompileGlob.codePos = value;
}

int CompareCaseInfo(const void *elem1, const void *elem2)
{
	if (static_cast<int32_t>(Scr_ReadSwitchKey(elem1)) > static_cast<int32_t>(Scr_ReadSwitchKey(elem2)))
	{
		return -1;
	}

	return static_cast<int32_t>(Scr_ReadSwitchKey(elem1)) < static_cast<int32_t>(Scr_ReadSwitchKey(elem2));
}

// Relative branches retain their four-byte operand on both ABIs. Patching a
// pointer-sized value here would overwrite the instruction after the branch.
void Scr_PatchJumpOffset(const char* operand, int32_t offset)
{
    std::memcpy(const_cast<char*>(operand), &offset, sizeof(offset));
}

void ConnectContinueStatements()
{
	const char *codePos = (char *)TempMalloc(0);

	for (ContinueStatementInfo *statement = scrCompileGlob.currentContinueStatement; statement; statement = statement->next)
	{
		Scr_PatchJumpOffset(statement->codePos, static_cast<int32_t>(codePos - statement->nextCodePos));
	}
}

void ConnectBreakStatements()
{
	iassert(!scrCompilePub.value_count);
	const char *codePos = (char *)TempMalloc(0);

	for (BreakStatementInfo *statement = scrCompileGlob.currentBreakStatement; statement; statement = statement->next)
	{
		Scr_PatchJumpOffset(statement->codePos, static_cast<int32_t>(codePos - statement->nextCodePos));
	}
}
