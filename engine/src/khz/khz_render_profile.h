#pragma once
#include <cstdint>
#include <atomic>
struct IDirect3DDevice9;
enum class KHZ_RenderStage : unsigned {
    Frontend, Consume, WorkerWait, FrontendWorkers, RendererWait, BackendIdle, Present,
    GpuSync, BeginFrame, Cgame, RenderScene, PacketEntities, ViewWeapon, Draw2D, DrawActive,
    VertexLock, IndexLock, WorldDpvs, Bsp, StaticModels, DynModels, SceneEntities, ModelLighting, MergeSurfs, Sound, FxPhysics, DynEntityPhysics, FrameWork, ClientFrame, EndFrame, BackendBegin, Backend3D, Backend2D, SwapFence, BackendGpuSync, DepthPass, LitPass, DecalPass, EmissivePass, LightPass, DrawSurfs, PostFx, DrawIndexed, StreamSource, IndexData, VertexConstants, PixelConstants, PixelShader, VertexShader, VertexDecl, SamplerTexture, SamplerState, SetupPass, TessWorld, TessStaticModel, TessModel, TessEffects, Count
};
#if defined(COD4X1KHZ) && !defined(KISAK_DEDICATED)
// Inline idle guards keep hot draw calls free of out-of-line timing calls.
extern std::atomic<bool> KHZ_RenderProfileRecording;
void KHZ_RenderProfileInit();
const char* KHZ_RenderProfileProbe(const char* action);
void KHZ_RenderProfileBackendBegin(IDirect3DDevice9* device);
void KHZ_RenderProfileDrawEnd();
void KHZ_RenderProfileFrameEnd();
// Called only after R_SyncRenderThread has transferred device ownership to main.
void KHZ_RenderProfileShutdown();
class KHZ_RenderProfileScope {
public:
    explicit KHZ_RenderProfileScope(KHZ_RenderStage stage, bool aggregate = false)
        : stage_(stage), aggregate_(aggregate) {
        if (KHZ_RenderProfileRecording.load(std::memory_order_relaxed)) beginCapture();
    }
    ~KHZ_RenderProfileScope() { if (begin_) endCapture(); }
    KHZ_RenderProfileScope(const KHZ_RenderProfileScope&) = delete;
    KHZ_RenderProfileScope& operator=(const KHZ_RenderProfileScope&) = delete;
private:
    void beginCapture();
    void endCapture();
    std::uint64_t begin_ = 0, epoch_ = 0;
    KHZ_RenderStage stage_;
    bool aggregate_;
};
#else
inline void KHZ_RenderProfileInit() {}
inline void KHZ_RenderProfileBackendBegin(IDirect3DDevice9*) {}
inline void KHZ_RenderProfileDrawEnd() {}
inline void KHZ_RenderProfileFrameEnd() {}
inline void KHZ_RenderProfileShutdown() {}
struct KHZ_RenderProfileScope { explicit KHZ_RenderProfileScope(KHZ_RenderStage, bool = false) {} };
#endif
