#include <universal/q_shared.h>
#include "scr_compiler.h"
#include "scr_parser.h"
#include "scr_stringlist.h"

uint32_t SpecifyThreadPosition(uint32_t posId, uint32_t name, uint32_t sourcePos, int type)
{
    const auto id = GetVariable(posId, 1);
    auto pos = Scr_EvalVariable(id);
    if (pos.type == VAR_UNDEFINED)
    {
        pos.type = static_cast<Vartype_t>(type);
        // Undefined variables can retain a previous value in their union.
        // This is a pointer placeholder: clearing an int leaves the upper
        // half live on x64, falsely resolving a forward function reference.
        pos.u.codePosValue = nullptr;
        SetNewVariableValue(id, &pos);
        return id;
    }
    if (pos.u.codePosValue)
        CompileError(sourcePos, "function '%s' already defined in '%s'", SL_ConvertToString(name),
            scrParserPub.sourceBufferLookup[Scr_GetSourceBuffer(pos.u.codePosValue)].buf);
    else
        CompileError(sourcePos, "function '%s' already defined", SL_ConvertToString(name));
    return 0;
}
