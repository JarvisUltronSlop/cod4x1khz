// Exercise the production allocator, clipped mark storage and index generation.
// Including this TU exposes its private helpers without shipping test hooks.
#include <EffectsCore/fx_marks.cpp>
#include <gfx_d3d/r_meshdata.h>
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <mutex>
#include <thread>
#include <barrier>

FxMarksSystem fx_marksSystemPool[1];
namespace {
alignas(16) GfxWorldVertex vertices[6144];
alignas(16) uint16_t indices[9216];
GfxMeshData mesh{};
struct Draw { uint16_t* indices; unsigned count; GfxMarkContext context; };
std::vector<Draw> draws;
std::mutex markMutex;
std::barrier clipBarrier(2);
FxMarkPoint* clipScratch[2]{};
FxSystem fixtureSystem{};
dvar_t enabledMarks{};
void resetMesh() {
    mesh = {}; mesh.vertSize = sizeof(GfxWorldVertex);
    mesh.vb.verts = reinterpret_cast<uint8_t*>(vertices); mesh.vb.total = sizeof(vertices);
    mesh.indices = indices; mesh.totalIndexCount = ARRAY_COUNT(indices);
    std::memset(vertices, 0xcd, sizeof(vertices)); std::fill(std::begin(indices), std::end(indices), 0xffff);
    draws.clear(); fx_marksSystemPool[0].hasCarryIndex = false;
}
}
void MyAssertHandler(const char* file, int line, int, const char* format, ...) {
    std::fprintf(stderr, "Production assertion %s:%d: ", file, line);
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args); std::abort();
}
void Sys_EnterCriticalSection(int) { markMutex.lock(); }
void Sys_LeaveCriticalSection(int) { markMutex.unlock(); }
int Sys_InterlockedIncrement(uint* value) { return ++*value; }
int Sys_InterlockedDecrement(uint* value) { return --*value; }
void Vec3Copy(const float* a, float* b) { std::memcpy(b, a, 3*sizeof(float)); }
void Vec3Sub(const float* a,const float* b,float* c) { for(int i=0;i<3;++i)c[i]=a[i]-b[i]; }
float Vec3LengthSq(const float* v) { return v[0]*v[0]+v[1]*v[1]+v[2]*v[2]; }
void Vec3Cross(const float* a,const float* b,float* c) {
    float v[]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};Vec3Copy(v,c);
}
float Vec3Dot(const float* a,const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
PackedTexCoords Vec2PackTexCoords(const float*) { return {}; }
char R_ReserveMarkMeshVerts(int count,uint16_t* base) { return R_ReserveMeshVerts(&mesh,count,base); }
char R_ReserveMarkMeshIndices(int count,r_double_index_t** out) { return R_ReserveMeshIndices(&mesh,count,out); }
GfxWorldVertex* R_GetMarkMeshVerts(uint16_t base) { return vertices+base; }
void R_AddMarkMeshDrawSurf(Material*, const GfxMarkContext* context, uint16_t* data, uint32_t count) {
    draws.push_back({data,count,*context});
}

int main() {
    auto& system=fx_marksSystemPool[0]; std::memset(&system,0,sizeof(system)); FX_InitMarksSystem(&system); resetMesh();
    // The union stride is 72 on x64, rather than the member's 68 bytes.
    auto* last=&system.pointGroups[FX_POINT_GROUP_LIMIT-1];
    const auto lastHandle=FX_PointGroupToHandle(&system,&last->pointGroup);
    assert(FX_PointGroupFromHandle(&system,lastHandle)==last);
    Material material{}; float origin[]={0,0,0}, axis[]={1,0,0}; uint8_t color[]={1,2,3,255};
    for(int count: {1,2,3,31,254,255}) {
        for(int type: {MARK_MODEL_TYPE_WORLD_BRUSH,MARK_MODEL_TYPE_WORLD_MODEL}) {
            std::memset(&system,0,sizeof(system)); FX_InitMarksSystem(&system); resetMesh();
            std::vector<FxMarkPoint> points(count*3);
            std::vector<FxMarkTri> triangles(count);
            for(int i=0;i<count;++i) {
                auto& t=triangles[i]; t.context.modelTypeAndSurf=type; t.context.modelIndex=0;
                t.context.lmapIndex=i%3; t.context.reflectionProbeIndex=i%4;
                for(int v=0;v<3;++v) {
                    const int n=i*3+v; t.indices[v]=n;
                    points[n].xyz[0]=n; points[n].xyz[1]=n*2; points[n].xyz[2]=n*3;
                    points[n].normal[2]=1; points[n].lmapCoord[0]=n*.001f;
                }
            }
            FX_AllocAndConstructMark(0,count,points.size(),&material,triangles.data(),points.data(),origin,100,axis,color);
            FxMark* mark=nullptr;
            for(auto& candidate:system.marks)if(candidate.frameCountDrawn!=FX_MARK_FREE)mark=&candidate;
            assert(mark); uint32_t total=0; uint16_t base=0; FxActiveMarkSurf surf{};
            assert(FX_GenerateMarkVertsForMark_Begin(&system,mark,&total,&base,&surf));
            if(type==MARK_MODEL_TYPE_WORLD_BRUSH)FX_ExpandMarkVerts_NoTransform_GfxWorldVertex_(&system,mark,base);
            else FX_ExpandMarkVerts_NoTransform_GfxPackedVertex_(&system,mark,base);
            R_AddMarkMeshDrawSurf(&material,&surf.context,surf.indices,surf.indexCount);
            FX_FinishGeneratingMarkVerts(&system);
            unsigned drawn=0;
            std::vector<bool> visited(count,false);
            for(const auto& draw:draws) {
                assert(draw.count%3==0); drawn+=draw.count;
                for(unsigned i=0;i<draw.count;i+=3) {
                    const unsigned triangle=draw.indices[i]/3;
                    assert(triangle<unsigned(count) && !visited[triangle]); visited[triangle]=true;
                    assert(draw.context.lmapIndex==triangle%3 && draw.context.reflectionProbeIndex==triangle%4);
                    for(unsigned v=0;v<3;++v)assert(draw.indices[i+v]==triangle*3+v);
                }
                for(unsigned i=0;i<draw.count;++i) {
                    const auto n=draw.indices[i]; assert(n<points.size());
                    for(unsigned j=0;j<3;++j) assert(vertices[n].xyz[j]==points[n].xyz[j]);
                }
            }
            assert(drawn==unsigned(count*3));
            assert(mesh.indexCount==unsigned((count*3+1)&~1));
        }
    }
    std::memset(&system,0,sizeof(system)); FX_InitMarksSystem(&system);
    FxMarkTri triangle{}; triangle.indices[1]=1; triangle.indices[2]=2;
    FxMarkPoint points[3]{};
    for(int i=0;i<800;++i)FX_AllocAndConstructMark(0,1,3,&material,&triangle,points,origin,1,axis,color);
    assert(system.allocedMarkCount-system.freedMarkCount==FX_MARKS_LIMIT);
    std::memset(&system,0,sizeof(system)); FX_InitMarksSystem(&system); resetMesh();
    uint32_t accumulated=0;
    for(unsigned m=0;m<2;++m) {
        FX_AllocAndConstructMark(0,1,3,&material,&triangle,points,origin,100,axis,color);
        uint16_t base=0;FxActiveMarkSurf surf{};
        assert(FX_GenerateMarkVertsForMark_Begin(&system,&system.marks[m],&accumulated,&base,&surf));
        FX_ExpandMarkVerts_NoTransform_GfxWorldVertex_(&system,&system.marks[m],base);
        R_AddMarkMeshDrawSurf(&material,&surf.context,surf.indices,surf.indexCount);
    }
    FX_FinishGeneratingMarkVerts(&system);
    assert(accumulated==6 && mesh.indexCount==6);
    for(unsigned i=0;i<6;++i)assert(indices[i]==i);
    resetMesh(); uint16_t base=0;
    assert(R_ReserveMeshVerts(&mesh,6144,&base)); const auto used=mesh.vb.used;
    assert(!R_ReserveMeshVerts(&mesh,1,&base)); assert(mesh.vb.used==used);
    assert(!R_ReserveMeshVerts(&mesh,INT_MAX,&base)); assert(mesh.vb.used==used);
    r_double_index_t* out=nullptr;
    assert(R_ReserveMeshIndices(&mesh,9216,&out));
    assert(!R_ReserveMeshIndices(&mesh,2,&out)); assert(mesh.indexCount==9216);
    std::memset(&system,0,sizeof(system)); FX_InitMarksSystem(&system);
    enabledMarks.current.enabled=true; fx_marks=&enabledMarks;
    fx_marks_ents=&enabledMarks; fx_marks_smodels=&enabledMarks;
    float axes[3][3]={{1,0,0},{0,1,0},{0,0,1}};
    auto impact=[&](float x) {
        float location[]={x,0,0};
        FX_ImpactMark_Generate(0,MARK_FRAGMENTS_AGAINST_BRUSHES,&material,location,axes,0,color,100,ENTITYNUM_NONE);
    };
    std::thread first(impact,10.f),second(impact,20.f); first.join();second.join();
    if(clipScratch[0]==clipScratch[1]) {std::fputs("Concurrent impacts shared clipping scratch\n",stderr);return 1;}
    assert(system.allocedMarkCount==2);
    for(auto& mark:system.marks)if(mark.frameCountDrawn!=FX_MARK_FREE) {
        auto* group=FX_PointGroupFromHandle(&system,mark.points);
        assert(group->pointGroup.points[0].xyz[0]==mark.origin[0]);
    }
    std::puts("Native concurrent impact scratch, decal pools, clipped geometry, index carry, eviction and mesh bounds passed");
}

// Unused engine entry points fail loudly if this fixture expands its scope.
char R_MarkFragments_AddDObj(MarkInfo *,DObj_s *,cpose_t *,unsigned short) { std::abort(); }
void FX_SetupVisualState(FxElemDef const *,FxEffect const *,int,float,FxElemPreVisualState *) { std::abort(); }
dvar_s const * fx_marks_smodels = nullptr;
dvar_s const * fx_marks_ents = nullptr;
void MatrixTransformVector(float const *,float const (&)[3][3],float *) { std::abort(); }
double DObjGetRadius(DObj_s const *) { std::abort(); }
void FX_EvaluateVisualState(FxElemPreVisualState *,float,FxElemVisualState *) { std::abort(); }
DObjAnimMat const * XModelGetBasePose(XModel const *) { std::abort(); }
DObj_s * Com_GetClientDObj(unsigned int,int) { std::abort(); }
void Vec3AddScalar(float const *,float,float *) { std::abort(); }
GfxBrushModel * R_GetBrushModel(unsigned int) { std::abort(); }
void Vec3Add(float const *,float const *,float *) { std::abort(); }
void UnitQuatToAxis(float const *,float (&)[3][3]) { std::abort(); }
void R_MarkFragments_Begin(MarkInfo* info,MarkFragmentsAgainstEnum,float const* origin,float const (*)[3],float,float const *,Material *) {
    *info={};Vec3Copy(origin,info->origin);
}
char * DObjGetModelParentBoneName(DObj_s const *,int) { std::abort(); }
double FX_InterpolateSize(FxElemVisStateSample const *,int,FxRandKey,float,float,int) { std::abort(); }
void MatrixTransformVector43(float const *,float const (&)[4][3],float *) { std::abort(); }
const float fx_randomTable[507] = {};
char R_MarkFragments_AddBModel(MarkInfo *,GfxBrushModel *,cpose_t *,unsigned short) { std::abort(); }
void R_MarkUtil_GetDObjAnimMatAndHideParts(DObj_s const *,cpose_t const *,DObjAnimMat const * *,unsigned int *) { std::abort(); }
char FX_CullSphere(FxCamera const *,unsigned int,float const *,float) { std::abort(); }
void RotatePointAroundVector(float *,float const *,float const *,float) { std::abort(); }
int DObjGetNumModels(DObj_s const *) { std::abort(); }
void DObjSkelMatToMatrix43(DObjSkelMat const *,float (*)[3]) { std::abort(); }
void R_EndMarkMeshVerts(void) { std::abort(); }
centity_s (&KHZ_CEntityArray(void))[1][1024] { std::abort(); }
void R_BeginMarkMeshVerts(void) { std::abort(); }
void MatrixMultiply43(float const (&)[4][3],float const (&)[4][3],float (&)[4][3]) { std::abort(); }
void R_MarkFragments_Go(MarkInfo* info,void (__cdecl*callback)(void *,int,FxMarkTri *,int,FxMarkPoint *,float const *,float const *),void* opaque,int,FxMarkTri* tris,int,FxMarkPoint* points) {
    clipScratch[info->origin[0]==10.f?0:1]=points;
    tris[0]={};tris[0].indices[1]=1;tris[0].indices[2]=2;
    for(int i=0;i<3;++i) {points[i]={};points[i].xyz[0]=info->origin[0]+i;points[i].normal[2]=1;}
    clipBarrier.arrive_and_wait();
    const float axis[]={1,0,0};callback(opaque,1,tris,3,points,info->origin,axis);
}
XModel * DObjGetModel(DObj_s const *,int) { std::abort(); }
int XModelNumBones(XModel const *) { std::abort(); }
FxSystem * FX_GetSystem(int) { return &fixtureSystem; }
void AnglesToAxis(float const *,float (* const)[3]) { std::abort(); }
void ConvertQuatToInverseSkelMat(DObjAnimMat const * const,DObjSkelMat *) { std::abort(); }
bool BoundsOverlap(float const *,float const *,float const *,float const *) { std::abort(); }
void Vec4Copy(float const *,float *) { std::abort(); }
dvar_s const * fx_marks = nullptr;
void track_static_alloc_internal(void *,int,char const *,int) { std::abort(); }
void ConvertQuatToSkelMat(DObjAnimMat const * const,DObjSkelMat *) { std::abort(); }
