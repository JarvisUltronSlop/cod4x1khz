#include <universal/q_shared.h>
#include <gfx_d3d/r_marks.h>
#include <cassert>
#include <cstdio>

namespace {
MarkModelCoreContext expected{};
const GfxPackedVertex* expectedVertices[3]{};
bool reject=false;
char triangleResult=0;
unsigned rejectedCalls=0,triangleCalls=0;
}
bool __cdecl R_MarkFragment_IsTriangleRejected(const float* direction,
    const float* first,const float* second,const float* third) {
    ++rejectedCalls;
    assert(direction==expected.markDir);
    const float* actual[]={first,second,third};
    for(unsigned vertex=0;vertex<3;++vertex)for(unsigned axis=0;axis<3;++axis)
        assert(actual[vertex][axis]==expectedVertices[vertex]->xyz[axis]);
    return reject;
}
char __cdecl R_MarkFragment_DoTriangle_0_(MarkInfo* info,
    const float (*planes)[4],const GfxMarkContext* context,
    const GfxPackedVertex** vertices,const float (*normals)[3],
    FxModelMarkPoint (*points)[9]) {
    ++triangleCalls;
    assert(info==expected.markInfo&&planes==expected.clipPlanes&&context==expected.markContext);
    assert(normals==expected.transformNormalMatrix);
    for(unsigned vertex=0;vertex<3;++vertex){
        assert(vertices[vertex]==expectedVertices[vertex]);
        for(unsigned axis=0;axis<3;++axis){
            assert(points[0][vertex].xyz[axis]==expectedVertices[vertex]->xyz[axis]);
            assert(points[0][vertex].vertWeights[axis]==(vertex==axis?1.f:0.f));
        }
    }
    return triangleResult;
}
int main(){
    MarkInfo info{};GfxMarkContext context{};
    float origin[3]={3,4,5},direction[3]={0,0,1},planes[9][4]{},matrix[4][3]{},normals[3][3]{};
    expected={&info,&context,origin,direction,planes,matrix,normals};
    GfxPackedVertex vertices[3]{};
    for(unsigned vertex=0;vertex<3;++vertex){
        for(unsigned axis=0;axis<3;++axis)vertices[vertex].xyz[axis]=float(vertex*10+axis);
        expectedVertices[vertex]=&vertices[vertex];
    }
    auto opaque=static_cast<void*>(&expected);
    assert(!R_MarkModelCoreCallback_0_(opaque,expectedVertices,expectedVertices));
    assert(rejectedCalls==1&&triangleCalls==1);
    triangleResult=1;
    assert(R_MarkModelCoreCallback_0_(opaque,expectedVertices,expectedVertices));
    assert(rejectedCalls==2&&triangleCalls==2);
    reject=true;
    assert(R_MarkModelCoreCallback_0_(opaque,expectedVertices,expectedVertices));
    assert(rejectedCalls==3&&triangleCalls==2);
    std::puts("Native animated-model decal callback context and triangle forwarding passed");
}
