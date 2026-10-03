#pragma once
#include <cstdint>
struct IDirect3DDevice9;
enum class KHZ_RenderStage : unsigned {
    Frontend, Consume, WorkerWait, FrontendWorkers, RendererWait, BackendIdle, Present,
    GpuSync, BeginFrame, Cgame, RenderScene, PacketEntities, ViewWeapon, Draw2D, DrawActive,
    VertexLock, IndexLock, WorldDpvs, Bsp, StaticModels, DynModels, SceneEntities, ModelLighting, MergeSurfs, Sound, FxPhysics, DynEntityPhysics, FrameWork, ClientFrame, EndFrame, Count
};
#if defined(COD4X1KHZ) && !defined(KISAK_DEDICATED)
void KHZ_RenderProfileInit();
const char* KHZ_RenderProfileProbe(const char* action);
void KHZ_RenderProfileBackendBegin(IDirect3DDevice9* device);
void KHZ_RenderProfileDrawEnd();
void KHZ_RenderProfileFrameEnd();
// Called only after R_SyncRenderThread has transferred device ownership to main.
void KHZ_RenderProfileShutdown();
class KHZ_RenderProfileScope {
public:
    explicit KHZ_RenderProfileScope(KHZ_RenderStage stage);
    ~KHZ_RenderProfileScope();
    KHZ_RenderProfileScope(const KHZ_RenderProfileScope&) = delete;
    KHZ_RenderProfileScope& operator=(const KHZ_RenderProfileScope&) = delete;
private:
    std::uint64_t begin_ = 0, epoch_ = 0;
    KHZ_RenderStage stage_;
};
#else
inline void KHZ_RenderProfileInit() {}
inline void KHZ_RenderProfileBackendBegin(IDirect3DDevice9*) {}
inline void KHZ_RenderProfileDrawEnd() {}
inline void KHZ_RenderProfileFrameEnd() {}
inline void KHZ_RenderProfileShutdown() {}
struct KHZ_RenderProfileScope { explicit KHZ_RenderProfileScope(KHZ_RenderStage) {} };
#endif
