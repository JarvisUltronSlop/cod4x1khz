#include "scr_animtree.h"
#include "scr_variable.h"
#include "scr_stringlist.h"
#include <qcommon/qcommon.h>
#include <cstring>
#include <unordered_map>

// Animation handles and OP_GetAnimation operands are four bytes on both ABIs.
// Keep unresolved links outside those slots: a native pointer cannot fit there.
static std::unordered_map<const char*, const char*> animationFixups;

void Scr_ClearAnimationFixups()
{
    animationFixups.clear();
}

void __cdecl Scr_EmitAnimationInternal(char* pos, uint32_t animName, uint32_t names)
{
    iassert(names);
    const auto animId = FindVariable(names, animName);
    const char* next = animId ? GetVariableValueAddress(animId)->u.codePosValue : nullptr;
    animationFixups[pos] = next;
    const scr_anim_s unresolved{};
    std::memcpy(pos, &unresolved, sizeof(unresolved));
    if (animId)
        GetVariableValueAddress(animId)->u.codePosValue = pos;
    else
    {
        VariableValue value{};
        value.type = VAR_CODEPOS;
        value.u.codePosValue = pos;
        SetVariableValue(GetNewVariable(names, animName), &value);
    }
}

void __cdecl ConnectScriptToAnim(uint32_t names, uint16_t index,
    uint32_t filename, uint32_t name, uint16_t treeIndex)
{
    const auto animId = FindVariable(names, name);
    if (!animId)
        return;
    auto* value = GetVariableValueAddress(animId);
    if (!value->u.codePosValue)
        Com_Error(ERR_DROP, "duplicate animation %s in animtrees/%s.atr",
            SL_ConvertToString(name), SL_ConvertToString(filename));
    scr_anim_s anim{};
    anim.index = index;
    anim.tree = treeIndex;
    for (const char* pos = value->u.codePosValue; pos; )
    {
        const auto found = animationFixups.find(pos);
        iassert(found != animationFixups.end());
        const char* next = found->second;
        std::memcpy(const_cast<char*>(pos), &anim, sizeof(anim));
        animationFixups.erase(found);
        pos = next;
    }
    value->u.codePosValue = nullptr;
}
