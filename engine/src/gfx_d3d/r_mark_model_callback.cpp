#include <universal/q_shared.h>
#include "r_marks.h"
#include <EffectsCore/fx_system.h>

bool __cdecl R_MarkModelCoreCallback_0_(
    void* contextAsVoid,
    const GfxPackedVertex** triVerts0,
    const GfxPackedVertex** triVerts1)
{
    const auto& context = *static_cast<const MarkModelCoreContext*>(contextAsVoid);
    FxModelMarkPoint clipPoints[2][9]{};
    for (unsigned vertex = 0; vertex != 3; ++vertex) {
        for (unsigned axis = 0; axis != 3; ++axis)
            clipPoints[0][vertex].xyz[axis] = triVerts0[vertex]->xyz[axis];
        clipPoints[0][vertex].vertWeights[vertex] = 1.0f;
    }
    return R_MarkFragment_IsTriangleRejected(context.markDir,
        clipPoints[0][0].xyz, clipPoints[0][1].xyz, clipPoints[0][2].xyz)
        || R_MarkFragment_DoTriangle_0_(context.markInfo, context.clipPlanes,
            context.markContext, triVerts1, context.transformNormalMatrix, clipPoints) != 0;
}
