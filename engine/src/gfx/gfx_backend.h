#pragma once
#include "graphics_resource_table.hpp"
#include <array>
#include <span>
#include <vector>
#include <string>

// This boundary contains neither COM interfaces nor 64-bit GPU handles. Stock
// fastfile structures stay unchanged; resource IDs refer to backend-owned tables.
namespace gfx {
enum class Api { D3D9, Vulkan };
enum class QueryType { Event, Occlusion, Timestamp };
enum class ImageFormat { Bgra8, Rgba8, Bc1, Bc2, Bc3, R8, Rg8, R32Float, DepthStencil, Alpha8, Rgba16Unorm, Rgba16Float };
enum class ImageKind { Texture2D, Cube, Volume };
enum class VertexFormat { Float, Float2, Float3, Float4, Color, Ubyte4, Short2, Short4, Short2N, Short4N, Half2, Half4, Ubyte4N, Ushort2N, Ushort4N, Udec3, Dec3N };
enum class Semantic { Position, BlendWeight, BlendIndices, Normal, PointSize, Texcoord, Tangent, Binormal, TessFactor, PositionT, Color, Fog, Depth, Sample };
enum class Compare { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Blend { Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DstAlpha, InvDstAlpha, DstColor, InvDstColor, SrcAlphaSaturate };
enum class BlendOp { Add, Subtract, ReverseSubtract, Min, Max };
enum class StencilOp { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
enum class Address { Repeat, Mirror, Clamp, Border };
struct ImageDesc {
    unsigned width = 1, height = 1, depth = 1, levels = 1, samples = 1;
    ImageFormat format = ImageFormat::Bgra8; ImageKind kind = ImageKind::Texture2D;
    bool renderTarget = false, sampled = true, srgb = false;
};
struct VertexElement {
    unsigned stream = 0, offset = 0, index = 0;
    VertexFormat format = VertexFormat::Float4; Semantic semantic = Semantic::Position;
    friend bool operator==(const VertexElement&, const VertexElement&) = default;
};
struct Sampler {
    Address u = Address::Repeat, v = Address::Repeat, w = Address::Repeat;
    bool linear = true, mipLinear = true, compare = false; Compare comparison = Compare::LessEqual;
    bool minLinear=true,magLinear=true;
    float maxAnisotropy = 1, lodBias = 0, minLod = 0, maxLod = 16;
    friend bool operator==(const Sampler&, const Sampler&) = default;
};
struct MaterialState {
    bool depthTest = true, depthWrite = true, blend = false, cull = true, clockwise = true;
    Compare depthCompare = Compare::LessEqual;
    Blend src = Blend::One, dst = Blend::Zero, srcAlpha = Blend::One, dstAlpha = Blend::Zero;
    BlendOp blendOp = BlendOp::Add, alphaBlendOp = BlendOp::Add;
    unsigned colorMask = 15;
    // Unsupported states fail explicitly until their shader/graphics lowering
    // is implemented, rather than silently changing a stock material's image.
    bool alphaTest = false, stencil = false, wireframe = false;
    Compare alphaCompare = Compare::Greater; unsigned alphaRef = 0;
    struct Stencil {
        Compare compare = Compare::Always;
        StencilOp fail = StencilOp::Keep, depthFail = StencilOp::Keep, pass = StencilOp::Keep;
        friend bool operator==(const Stencil&,const Stencil&) = default;
    } front, back;
    unsigned stencilRef = 0, stencilReadMask = 255, stencilWriteMask = 255;
    float depthBias=0,slopeDepthBias=0;
    friend bool operator==(const MaterialState&, const MaterialState&) = default;
};
struct DrawPacket {
    ResourceId vertexShader, pixelShader, indexBuffer, colorTarget, depthTarget;
    std::array<ResourceId, 16> vertexBuffers{}, textures{};
    std::array<unsigned, 16> strides{}, vertexOffsets{};
    std::array<Sampler, 16> samplers{};
    std::span<const VertexElement> elements;
    std::span<const float> vertexConstants, pixelConstants;
    std::span<const int> vertexIntegers, pixelIntegers, vertexBooleans, pixelBooleans;
    // Opt-in only for versioned packet storage that cannot change until the
    // current recording ends. Ordinary callers may mutate spans between draws.
    bool immutableConstants=false;
    // The legacy device captures a monotonically growing written prefix.
    // Registers beyond each span have never been set and remain zero.
    bool zeroConstantTail=false;
    // Optional producer-owned monotonically increasing version of the complete
    // pipeline key. Nonzero equal versions within one immutable submission
    // promise identical shaders, declaration/strides, targets, material state
    // and depth-sampler mask. Ordinary backend callers leave this zero.
    std::uint64_t immutablePipelineVersion=0;
    // Equal nonzero versions in one submission promise identical shader,
    // texture-resource and sampler bindings, independently of uniform values.
    std::uint64_t immutableDescriptorVersion=0;
    MaterialState state;
    unsigned indexCount = 0, firstIndex = 0; int vertexBase = 0;
    unsigned vertexFirst=0,vertexCount=0;
    float x = 0, y = 0, width = 1, height = 1, minDepth = 0, maxDepth = 1;
    bool index32 = false;
    bool scissor=false;int scissorX=0,scissorY=0;unsigned scissorWidth=0,scissorHeight=0;
};
struct FrameTiming {
    std::uint64_t serial = 0;
    std::uint64_t gpuSerial = 0;
    double gpuMs = 0, recordMs = 0, submitMs = 0, acquireMs = 0, presentMs = 0, recycleWaitMs = 0, swapchainMs = 0;
    // prepareMs is recordMs minus command-pass time, including CPU replay,
    // uploads, queries, barriers and frame finish; no per-draw clock sampling.
    double prepareMs=0,commandsMs=0;
    unsigned parallelBatches=0,parallelDraws=0;
    unsigned parallelPrepareBatches=0,parallelPrepareDraws=0,batchedDraws=0;
    std::uint64_t geometryCopiedBytes=0,geometryReusedBytes=0;
    unsigned draws = 0;
};
struct Config {
    void* window = nullptr; int device = -1; unsigned framesInFlight = 2;
    bool validation = false, vsync = false;
    bool compareD3D9 = false; // isolated immutable-packet visual diagnostic
    bool cacheBindings = true; // development A/B control
    bool cacheDescriptors = true;
    bool parallelPrepare = true,cacheGeometry = true,batchDraws = true;
    bool overlapFrontendUploads = false;
    bool asyncSubmission = false;
    void (*initializeSubmissionThread)() = nullptr;
    void (*releaseSubmissionThread)() = nullptr;
    unsigned recordingWorkers = 1;
    bool recordFrameTimings=false; // bounded isolated benchmark capture
    // Synchronous dispatch into the engine's existing renderer worker pool.
    // Each numbered task executes exactly once; all finish before returning.
    void (*dispatchRecording)(void (*task)(void*,unsigned),void* data,unsigned count)=nullptr;
    std::string cacheDirectory;
};
struct ClearRect { int left=0,top=0,right=0,bottom=0; };
class Backend {
public:
    virtual ~Backend() = default;
    virtual Api api() const noexcept = 0;
    virtual ResourceId createBuffer(std::span<const std::byte> data, bool index, bool dynamic = false) = 0;
    virtual void updateBuffer(ResourceId buffer,unsigned offset,std::span<const std::byte> data,bool discard=false,bool noOverwrite=false) = 0;
    virtual ResourceId createImage(const ImageDesc& desc) = 0;
    virtual void uploadImage(ResourceId image, unsigned level, unsigned layer, std::span<const std::byte> data) = 0;
    virtual ResourceId createShader(std::span<const std::uint32_t> bytecode, std::string name) = 0;
    virtual ResourceId createQuery(QueryType type) = 0;
    virtual void beginQuery(ResourceId query) = 0;
    virtual void endQuery(ResourceId query) = 0;
    // false means genuinely pending; zero is a valid completed sample count.
    virtual bool queryResult(ResourceId query, std::uint64_t& value) = 0;
    virtual void release(ResourceId resource) = 0;
    virtual void beginFrame() = 0;
    virtual void clear(ResourceId color, ResourceId depth, const std::array<float, 4>& rgba, float depthValue = 1,
        unsigned stencilValue=0,bool clearDepth=true,bool clearStencil=true,std::span<const ClearRect> rectangles={}) = 0;
    virtual void draw(const DrawPacket& packet) = 0;
    virtual void warm(const DrawPacket& packet) = 0;
    virtual void resolve(ResourceId source,ResourceId destination) = 0;
    virtual void blit(ResourceId source,ResourceId destination,bool linear=false,
        const ClearRect* sourceRect=nullptr,const ClearRect* destinationRect=nullptr) = 0;
    virtual FrameTiming endFrame(ResourceId presentationSource = {}) = 0;
    virtual std::vector<std::byte> readback(ResourceId image) = 0;
    virtual void waitIdle() = 0;
    virtual std::string deviceReport() const = 0;
};
}
