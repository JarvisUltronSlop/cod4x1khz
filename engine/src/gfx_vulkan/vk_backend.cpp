#include "vk_backend.h"
#include "vk_shader_cache.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <exception>
#include <tuple>

namespace gfx::vk {
namespace {
constexpr unsigned maxResources = 16384, maxDraws = 8192;
constexpr VkDeviceSize arenaBytes = 32 * 1024 * 1024;
// Fixed open-addressed indexes: no per-draw nodes or allocations. Full keys
// are compared after hashing, so collisions cannot change material semantics.
constexpr unsigned cacheSlots=maxDraws*2;
struct KeyHash {
    unsigned value=2166136261u;
    void add(unsigned v){value=(value^v)*16777619u;}
    template<class E> void enumeration(E v){add(unsigned(v));}
    void real(float v){add(v==0.f?0u:std::bit_cast<unsigned>(v));}
    void material(const MaterialState& s){
        add(s.depthTest);add(s.depthWrite);add(s.blend);add(s.cull);add(s.clockwise);
        enumeration(s.depthCompare);enumeration(s.src);enumeration(s.dst);enumeration(s.srcAlpha);enumeration(s.dstAlpha);
        enumeration(s.blendOp);enumeration(s.alphaBlendOp);add(s.colorMask);add(s.alphaTest);add(s.stencil);add(s.wireframe);
        enumeration(s.alphaCompare);add(s.alphaRef);
        for(const auto* face:{&s.front,&s.back}){enumeration(face->compare);enumeration(face->fail);enumeration(face->depthFail);enumeration(face->pass);}
        add(s.stencilRef);add(s.stencilReadMask);add(s.stencilWriteMask);real(s.depthBias);real(s.slopeDepthBias);
    }
    void sampler(const Sampler& s){enumeration(s.u);enumeration(s.v);enumeration(s.w);add(s.linear);add(s.mipLinear);add(s.compare);enumeration(s.comparison);add(s.minLinear);add(s.magLinear);real(s.maxAnisotropy);real(s.lodBias);real(s.minLod);real(s.maxLod);}
};
unsigned pipelineHash(ResourceId vs,ResourceId ps,const MaterialState& state,
    const std::array<unsigned,16>& strides,std::span<const VertexElement> elements,VkFormat color,VkFormat depth,unsigned samples,unsigned shadowMask){
    KeyHash h;h.add(vs.value);h.add(ps.value);h.material(state);for(auto v:strides)h.add(v);
    h.add(unsigned(elements.size()));for(const auto& e:elements){h.add(e.stream);h.add(e.offset);h.add(e.index);h.enumeration(e.format);h.enumeration(e.semantic);}
    h.enumeration(color);h.enumeration(depth);h.add(samples);h.add(shadowMask);return h.value;
}
unsigned descriptorHash(ResourceId vs,ResourceId ps,const std::array<ResourceId,16>& textures,const std::array<Sampler,16>& samplers,unsigned mask){
    KeyHash h;h.add(vs.value);h.add(ps.value);for(unsigned slot=0;slot<16;++slot)if(mask&(1u<<slot)){h.add(textures[slot].value);h.sampler(samplers[slot]);}return h.value;
}
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
VkFormat format(ImageFormat value, bool srgb, const Context& context) {
    switch (value) {
    case ImageFormat::Bgra8: return srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    case ImageFormat::Rgba8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case ImageFormat::Bc1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case ImageFormat::Bc2: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case ImageFormat::Bc3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case ImageFormat::R8: case ImageFormat::Alpha8: return VK_FORMAT_R8_UNORM;
    case ImageFormat::Rg8: return VK_FORMAT_R8G8_UNORM;
    case ImageFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
    case ImageFormat::Rgba16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case ImageFormat::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case ImageFormat::DepthStencil: return context.depthFormat();
    }
    throw std::runtime_error("Unsupported stock image format");
}
std::size_t imageBytes(const ImageDesc& d, unsigned level) {
    const auto w = (std::max)(1u, d.width >> level), h = (std::max)(1u, d.height >> level), z = (std::max)(1u, d.depth >> level);
    switch (d.format) {
    case ImageFormat::Bc1: return std::size_t((w + 3) / 4) * ((h + 3) / 4) * z * 8;
    case ImageFormat::Bc2: case ImageFormat::Bc3: return std::size_t((w + 3) / 4) * ((h + 3) / 4) * z * 16;
    case ImageFormat::R8: case ImageFormat::Alpha8: return std::size_t(w) * h * z;
    case ImageFormat::Rg8: return std::size_t(w) * h * z * 2;
    case ImageFormat::Rgba16Unorm: case ImageFormat::Rgba16Float: return std::size_t(w) * h * z * 8;
    case ImageFormat::DepthStencil: throw std::runtime_error("Depth image upload/readback needs an explicit aspect");
    default: return std::size_t(w) * h * z * 4;
    }
}
VkFormat vertexFormat(VertexFormat f) {
    const VkFormat map[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_A2B10G10R10_UINT_PACK32,VK_FORMAT_A2B10G10R10_SNORM_PACK32};
    if (unsigned(f) >= std::size(map)) throw std::runtime_error("Unsupported vertex format"); return map[unsigned(f)];
}
VkBlendFactor blend(Blend f) {
    const VkBlendFactor map[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (unsigned(f) >= std::size(map)) throw std::runtime_error("Unsupported blend factor"); return map[unsigned(f)];
}
VkSamplerAddressMode address(Address a) {
    const VkSamplerAddressMode map[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    if (unsigned(a) >= std::size(map)) throw std::runtime_error("Unsupported sampler address mode"); return map[unsigned(a)];
}
// These ordinal maps deliberately mirror the neutral boundary's enums.
static_assert(unsigned(Compare::Always) == VK_COMPARE_OP_ALWAYS && unsigned(BlendOp::Max) == VK_BLEND_OP_MAX);
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE; VmaAllocation allocation = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize bytes = 0;
};
struct Image {
    ImageDesc desc; VkImage image = VK_NULL_HANDLE; VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE, sampledView = VK_NULL_HANDLE; VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    unsigned layers = 1;
};
struct Resource {
    Buffer buffer; Image image; std::unique_ptr<Shader> shader;
    std::uint64_t lastUse = 0;
    std::vector<std::byte> dynamicBytes;
    struct GeometryVersion {Buffer buffer;std::uint64_t revision=0,serial=0;};
    std::array<GeometryVersion,3> geometryVersions;
    VkBufferUsageFlags geometryUsage=0;
    VkBuffer uploadedBuffer=VK_NULL_HANDLE;void* uploadedMapped=nullptr;
    std::uint64_t revision=0,uploadedRevision=0,uploadedSerial=0;
    VkDeviceSize uploadedOffset=0;
    unsigned usedBytes=0;
    std::uint64_t readBegin=UINT64_MAX,readEnd=0;
    struct Query {
        VkQueryPool pool=VK_NULL_HANDLE; QueryType type=QueryType::Event;
        struct Slice {std::uint64_t serial=0;unsigned count=0;};
        std::array<Slice,3> slices{};unsigned selected=0;bool issued=false;
    } query;
};
struct Pipeline {
    ResourceId vs, ps; std::vector<VertexElement> elements; std::array<unsigned, 16> strides{};
    MaterialState state; VkFormat color, depth; unsigned samples;
    unsigned depthSamplerMask=0;
    VkPipeline pipeline = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, 4> sets{};
    std::uint64_t lastUse = 0;
};
struct Frame {
    struct Recording {
        VkCommandPool pool=VK_NULL_HANDLE;
        std::array<VkCommandBuffer,128> commands{};
        unsigned used=0;
    };
    std::array<Recording,4> workers;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE; VkQueryPool queries = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkDeviceSize cachedGeometryBytes=0;
    Buffer arena; VkDeviceSize used = 0; std::uint64_t serial = 0; unsigned draws = 0;
    struct Descriptors {ResourceId vs,ps;unsigned mask=0;std::array<ResourceId,16> textures{};std::array<Sampler,16> samplers{};std::array<VkDescriptorSet,4> sets{};};
    std::vector<Descriptors> cachedDescriptors;
    std::array<unsigned,cacheSlots> descriptorIndex{};
    bool descriptorsDirty=false;
    struct Uniform {ResourceId shader;std::uint64_t serial=0;std::span<const float> floats;std::span<const int> integers,booleans;unsigned offset=0;bool zeroTail=false;};
    std::vector<Uniform> uniforms;
    struct Bindings {
        VkPipeline pipeline=VK_NULL_HANDLE;VkPipelineLayout descriptors=VK_NULL_HANDLE,push=VK_NULL_HANDLE;
        std::array<VkDescriptorSet,4> sets{};std::array<unsigned,2> offsets{};unsigned offsetCount=0;
        std::array<VkBuffer,16> vertices{};std::array<VkDeviceSize,16> vertexOffsets{};
        VkBuffer index=VK_NULL_HANDLE;VkDeviceSize indexOffset=0;VkIndexType indexType=VK_INDEX_TYPE_UINT16;
        VkViewport viewport{};VkRect2D scissor{};std::array<float,3> constants{};
        unsigned stencil=0;bool viewportValid=false,scissorValid=false,stencilValid=false;
    } bindings;
};
struct PreparedDraw {
    VkPipeline pipeline=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,4> sets{};std::array<unsigned,2> offsets{};unsigned offsetCount=0;
    std::array<VkBuffer,16> vertices{};std::array<VkDeviceSize,16> vertexOffsets{};
    VkBuffer index=VK_NULL_HANDLE;VkDeviceSize indexOffset=0;VkIndexType indexType=VK_INDEX_TYPE_UINT16;
    VkViewport viewport{};VkRect2D scissor{};std::array<float,3> constants{};
    bool stencil=false,push=false;unsigned stencilRef=0,indexCount=0,firstIndex=0;int vertexBase=0;
    VkQueryPool query=VK_NULL_HANDLE;unsigned queryIndex=0;VkQueryControlFlags queryFlags=0;
};
bool adjacentDraws(const PreparedDraw& a,const PreparedDraw& b){
    return !a.query&&!b.query&&a.indexCount%3==0&&b.indexCount%3==0&&
        std::uint64_t(a.firstIndex)+a.indexCount==b.firstIndex&&std::uint64_t(a.indexCount)+b.indexCount<=UINT32_MAX&&
        a.pipeline==b.pipeline&&a.layout==b.layout&&a.sets==b.sets&&a.offsets==b.offsets&&a.offsetCount==b.offsetCount&&
        a.vertices==b.vertices&&a.vertexOffsets==b.vertexOffsets&&a.index==b.index&&a.indexOffset==b.indexOffset&&a.indexType==b.indexType&&a.vertexBase==b.vertexBase&&
        a.stencil==b.stencil&&a.stencilRef==b.stencilRef&&a.push==b.push&&a.constants==b.constants&&
        !std::memcmp(&a.viewport,&b.viewport,sizeof(a.viewport))&&!std::memcmp(&a.scissor,&b.scissor,sizeof(a.scissor));
}
void rasterState(PreparedDraw& packet,const DrawPacket& d,unsigned width,unsigned height,bool bias){
    packet.stencil=d.state.stencil;packet.stencilRef=d.state.stencilRef;packet.push=d.state.alphaTest||bias;
    packet.constants={float(d.state.alphaRef)/255.f,d.state.depthBias,d.state.slopeDepthBias};
    packet.viewport={d.x+.5f,d.y+d.height+.5f,d.width,-d.height,d.minDepth,d.maxDepth};
    packet.scissor={{0,0},{width,height}};
    if(d.scissor){
        const auto left=std::clamp(d.scissorX,0,int(width)),top=std::clamp(d.scissorY,0,int(height));
        const auto right=std::clamp(std::int64_t(d.scissorX)+d.scissorWidth,std::int64_t(left),std::int64_t(width));
        const auto bottom=std::clamp(std::int64_t(d.scissorY)+d.scissorHeight,std::int64_t(top),std::int64_t(height));
        packet.scissor={{left,top},{unsigned(right-left),unsigned(bottom-top)}};
    }
    packet.indexCount=d.indexCount;packet.firstIndex=d.firstIndex;packet.vertexBase=d.vertexBase;
}
void recordDraw(Context& context,VkCommandBuffer cmd,const PreparedDraw& d,Frame::Bindings& bound,bool cacheBindings){
    if(!cacheBindings)bound={};
    if(d.query)context.api.CmdBeginQuery(cmd,d.query,d.queryIndex,d.queryFlags);
    if(bound.pipeline!=d.pipeline){context.api.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,d.pipeline);bound.pipeline=d.pipeline;}
    if(d.stencil&&(!bound.stencilValid||bound.stencil!=d.stencilRef)){context.api.CmdSetStencilReference(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,d.stencilRef);bound.stencil=d.stencilRef;bound.stencilValid=true;}
    if(d.push&&(bound.push!=d.layout||bound.constants!=d.constants)){context.api.CmdPushConstants(cmd,d.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,12,d.constants.data());bound.push=d.layout;bound.constants=d.constants;}
    if(!bound.viewportValid||std::memcmp(&bound.viewport,&d.viewport,sizeof(d.viewport))){context.api.CmdSetViewport(cmd,0,1,&d.viewport);bound.viewport=d.viewport;bound.viewportValid=true;}
    if(!bound.scissorValid||std::memcmp(&bound.scissor,&d.scissor,sizeof(d.scissor))){context.api.CmdSetScissor(cmd,0,1,&d.scissor);bound.scissor=d.scissor;bound.scissorValid=true;}
    if(bound.descriptors!=d.layout||bound.sets!=d.sets||bound.offsetCount!=d.offsetCount||bound.offsets!=d.offsets){context.api.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,d.layout,0,4,d.sets.data(),d.offsetCount,d.offsets.data());bound.descriptors=d.layout;bound.sets=d.sets;bound.offsetCount=d.offsetCount;bound.offsets=d.offsets;}
    for(unsigned s=0;s<16;++s)if(d.vertices[s]&&(bound.vertices[s]!=d.vertices[s]||bound.vertexOffsets[s]!=d.vertexOffsets[s])){context.api.CmdBindVertexBuffers(cmd,s,1,&d.vertices[s],&d.vertexOffsets[s]);bound.vertices[s]=d.vertices[s];bound.vertexOffsets[s]=d.vertexOffsets[s];}
    if(bound.index!=d.index||bound.indexOffset!=d.indexOffset||bound.indexType!=d.indexType){context.api.CmdBindIndexBuffer(cmd,d.index,d.indexOffset,d.indexType);bound.index=d.index;bound.indexOffset=d.indexOffset;bound.indexType=d.indexType;}
    context.api.CmdDrawIndexed(cmd,d.indexCount,1,d.firstIndex,d.vertexBase,0);
    if(d.query)context.api.CmdEndQuery(cmd,d.query,d.queryIndex);
}
}

struct Backend::Impl {
    Context context;
    gfx::Config config;
    std::thread::id owner = std::this_thread::get_id();
    ResourceTable<Resource, maxResources> resources;
    struct Retired { std::uint64_t serial; std::unique_ptr<Resource> resource; };
    std::vector<Retired> retired;
    std::vector<Pipeline> pipelines;
    std::array<unsigned,cacheSlots> pipelineIndex{};
    std::vector<Pipeline> retiredPipelines;
    struct CachedSampler { Sampler desc; VkSampler sampler; };
    std::vector<CachedSampler> samplers;
    std::array<Frame, 3> frames;
    Frame* frame = nullptr; unsigned frameIndex = 0; std::uint64_t nextSerial = 0,lastSubmitted=0;
    bool recordingFailed=false;
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::filesystem::path cachePath;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkSemaphore> presented;
    std::vector<bool> swapInitialized;
    VkExtent2D swapExtent{}; VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool swapDirty = true;
    FrameTiming timing;
    Clock::time_point recording;
    ResourceId activeOcclusion;
    ResourceId renderingColor,renderingDepth;
    std::vector<PreparedDraw> prepared;
    struct DrawPlan {
        const DrawPacket* source=nullptr;Shader* vertex=nullptr;Shader* pixel=nullptr;
        Image* color=nullptr;Image* depth=nullptr;Pipeline* pipeline=nullptr;
        unsigned samplerMask=0,descriptorHash=0;PreparedDraw packet;
    };
    struct UniformJob {
        const Shader* shader;unsigned offset;std::span<const float> floats;
        std::span<const int> integers,booleans;bool zeroTail;
    };
    std::vector<DrawPlan> plans;std::vector<UniformJob> uniformJobs;unsigned planCursor=0;
    std::uint64_t previousDescriptorVersion=0;Frame::Descriptors* previousDescriptors=nullptr;

    explicit Impl(const gfx::Config& c) : context({static_cast<HWND>(c.window), c.device, c.validation}), config(c) {
        if (c.framesInFlight < 1 || c.framesInFlight > 3) throw std::runtime_error("Frames in flight must be between 1 and 3");
        if(c.recordingWorkers<1||c.recordingWorkers>4)throw std::runtime_error("Recording workers must be between one and four");
        retired.reserve(maxResources); pipelines.reserve(maxDraws); retiredPipelines.reserve(maxDraws); samplers.reserve(512);
        prepared.reserve(maxDraws);plans.reserve(maxDraws);uniformJobs.reserve(maxDraws*2);
        try {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sem.pNext = &type;
            check(context.api.CreateSemaphore(context.device, &sem, nullptr, &timeline), "create resource retirement timeline");
            makeCache();
            for (unsigned i = 0; i < c.framesInFlight; ++i) {
                auto& f = frames[i]; VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = context.selected.queueFamily;
                f.cachedDescriptors.reserve(maxDraws);
                f.uniforms.resize(maxResources);
                check(context.api.CreateCommandPool(context.device, &pool, nullptr, &f.pool), "create frame command pool");
                VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; alloc.commandPool = f.pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
                check(context.api.AllocateCommandBuffers(context.device, &alloc, &f.cmd), "allocate frame command buffer");
                if(c.dispatchRecording&&c.recordingWorkers>1)for(unsigned worker=0;worker<c.recordingWorkers;++worker){
                    auto& r=f.workers[worker];check(context.api.CreateCommandPool(context.device,&pool,nullptr,&r.pool),"create independent worker command pool");
                    alloc.commandPool=r.pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;alloc.commandBufferCount=unsigned(r.commands.size());
                    check(context.api.AllocateCommandBuffers(context.device,&alloc,r.commands.data()),"allocate bounded worker command buffers");
                }
                const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxDraws * 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, maxDraws * 2}};
                VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descriptors.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; descriptors.maxSets = maxDraws * 4; descriptors.poolSizeCount = 2; descriptors.pPoolSizes = sizes;
                check(context.api.CreateDescriptorPool(context.device, &descriptors, nullptr, &f.descriptors), "create bounded frame descriptors");
                VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; queries.queryType = VK_QUERY_TYPE_TIMESTAMP; queries.queryCount = 2;
                check(context.api.CreateQueryPool(context.device, &queries, nullptr, &f.queries), "create frame GPU timestamps");
                sem.pNext = nullptr; check(context.api.CreateSemaphore(context.device, &sem, nullptr, &f.acquired), "create frame acquire semaphore");
                f.arena = makeBuffer(arenaBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            }
        } catch (...) { cleanup(); throw; }
    }
    void assertOwner() const { if (std::this_thread::get_id() != owner) throw std::runtime_error("Graphics submission owner violated"); }
    Buffer makeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage) {
        Buffer b; b.bytes = bytes;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = bytes; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo alloc{};
        alloc.usage=VMA_MEMORY_USAGE_AUTO;
        alloc.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo result{};
        check(vmaCreateBuffer(context.allocator, &info, &alloc, &b.buffer, &b.allocation, &result), "allocate Vulkan upload/buffer memory"); b.mapped = result.pMappedData;
        return b;
    }
    void destroy(Buffer& b) { if (b.buffer) vmaDestroyBuffer(context.allocator, b.buffer, b.allocation); b = {}; }
    void destroy(Resource& r) {
        if(r.query.pool) context.api.DestroyQueryPool(context.device,r.query.pool,nullptr);
        for(unsigned n=0;n<r.geometryVersions.size();++n){auto& v=r.geometryVersions[n];frames[n].cachedGeometryBytes-=v.buffer.bytes;destroy(v.buffer);}
        destroy(r.buffer);
        if (r.image.view) context.api.DestroyImageView(context.device, r.image.view, nullptr);
        if (r.image.sampledView) context.api.DestroyImageView(context.device, r.image.sampledView, nullptr);
        if (r.image.image) vmaDestroyImage(context.allocator, r.image.image, r.image.allocation);
        r.image = {}; r.shader.reset();
    }
    void wait(std::uint64_t serial) {
        if (!serial) return;
        std::uint64_t completed=0;check(context.api.GetSemaphoreCounterValue(context.device,timeline,&completed),"check frame resource completion");
        if(completed>=serial)return;
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO}; info.semaphoreCount = 1; info.pSemaphores = &timeline; info.pValues = &serial;
        check(context.api.WaitSemaphores(context.device, &info, UINT64_MAX), "wait frame resource completion");
    }
    void collect() {
        std::uint64_t complete = 0; check(context.api.GetSemaphoreCounterValue(context.device, timeline, &complete), "read completion timeline");
        for (auto it = retired.begin(); it != retired.end();) {
            if (it->serial <= complete) { destroy(*it->resource); it = retired.erase(it); } else ++it;
        }
        for(auto it=retiredPipelines.begin();it!=retiredPipelines.end();) {
            if(it->lastUse<=complete) {destroy(*it);it=retiredPipelines.erase(it);}else ++it;
        }
    }
    void destroy(Pipeline& p) {
        if(p.pipeline) context.api.DestroyPipeline(context.device,p.pipeline,nullptr);
        if(p.layout) context.api.DestroyPipelineLayout(context.device,p.layout,nullptr);
        for(auto set:p.sets) if(set) context.api.DestroyDescriptorSetLayout(context.device,set,nullptr);
    }
    Resource& use(ResourceId id) { auto& r = resources.get(id); r.lastUse = nextSerial; return r; }
    Image& image(ResourceId id) { auto& r = use(id); if (!r.image.image) throw std::runtime_error("Graphics reference is not an image"); return r.image; }
    Shader& shader(ResourceId id) { auto& r = use(id); if (!r.shader) throw std::runtime_error("Graphics reference is not a shader"); return *r.shader; }
    std::pair<VkBuffer,VkDeviceSize> geometry(Resource& r) {
        if(!r.dynamicBytes.empty()) {
            if(r.uploadedSerial!=nextSerial||r.uploadedRevision!=r.revision) {
                if(!r.usedBytes) throw std::runtime_error("Uninitialized dynamic geometry buffer");
                auto& version=r.geometryVersions[frameIndex];
                // A completed frame slot may reuse or replace its own version.
                // A second version inside this submission uses the arena so an
                // earlier draw can never observe overwritten geometry.
                if(config.cacheGeometry&&version.serial!=nextSerial&&
                    (version.buffer.buffer||r.buffer.bytes<=arenaBytes-frame->cachedGeometryBytes)){
                    if(!version.buffer.buffer){version.buffer=makeBuffer(r.buffer.bytes,r.geometryUsage);frame->cachedGeometryBytes+=r.buffer.bytes;}
                    if(version.revision!=r.revision){
                        std::memcpy(version.buffer.mapped,r.dynamicBytes.data(),r.usedBytes);check(vmaFlushAllocation(context.allocator,version.buffer.allocation,0,r.usedBytes),"flush cached geometry version");
                        version.revision=r.revision;timing.geometryCopiedBytes+=r.usedBytes;
                    }else timing.geometryReusedBytes+=r.usedBytes;
                    version.serial=nextSerial;r.uploadedBuffer=version.buffer.buffer;r.uploadedMapped=version.buffer.mapped;r.uploadedOffset=0;
                }else{
                    r.uploadedOffset=allocate(r.buffer.bytes,16);r.uploadedBuffer=frame->arena.buffer;
                    r.uploadedMapped=static_cast<std::byte*>(frame->arena.mapped)+r.uploadedOffset;
                    std::memcpy(r.uploadedMapped,r.dynamicBytes.data(),r.usedBytes);timing.geometryCopiedBytes+=r.usedBytes;
                }
                r.uploadedSerial=nextSerial;r.uploadedRevision=r.revision;
                r.readBegin=UINT64_MAX;r.readEnd=0;
            }
            return {r.uploadedBuffer,r.uploadedOffset};
        }
        if(!r.buffer.buffer) throw std::runtime_error("Graphics reference is not geometry");return {r.buffer.buffer,0};
    }
    void requireFrame() { assertOwner(); if (!frame) throw std::runtime_error("Vulkan work outside a frame"); }
    VkDeviceSize allocate(VkDeviceSize bytes, VkDeviceSize alignment) {
        const auto offset = (frame->used + alignment - 1) & ~(alignment - 1);
        if (bytes > arenaBytes || offset > arenaBytes - bytes) throw std::runtime_error("Bounded frame upload arena exhausted: request="+std::to_string(bytes)+" used="+std::to_string(frame->used)+" capacity="+std::to_string(arenaBytes));
        frame->used = offset + bytes; return offset;
    }
    void endRendering() {
        if(!renderingColor)return;
        const auto started=Clock::now();
        auto& color=resources.get(renderingColor).image;
        auto* depth=renderingDepth?&resources.get(renderingDepth).image:nullptr;
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment.imageView=color.view;attachment.imageLayout=color.layout;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo ds{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};if(depth){ds.imageView=depth->view;ds.imageLayout=depth->layout;ds.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;ds.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};rendering.renderArea.extent={color.desc.width,color.desc.height};rendering.layerCount=1;rendering.colorAttachmentCount=1;rendering.pColorAttachments=&attachment;rendering.pDepthAttachment=rendering.pStencilAttachment=depth?&ds:nullptr;
        unsigned workers=config.dispatchRecording?(std::min)(config.recordingWorkers,unsigned(prepared.size()/64)):1;
        if(workers<2||std::any_of(prepared.begin(),prepared.end(),[](const auto& d){return d.query!=VK_NULL_HANDLE;}))workers=1;
        if(workers>1){
            struct Batch {
                Impl* self;unsigned workers;VkFormat color,depth;VkSampleCountFlagBits samples;
                std::array<VkCommandBuffer,4> commands{};std::array<std::exception_ptr,4> errors{};
            } batch{this,workers,color.format,depth?depth->format:VK_FORMAT_UNDEFINED,VkSampleCountFlagBits(color.desc.samples)};
            for(unsigned n=0;n<workers;++n){auto& r=frame->workers[n];if(r.used==r.commands.size())throw std::runtime_error("Bounded secondary command buffers exhausted");batch.commands[n]=r.commands[r.used++];}
            const auto task=[](void* data,unsigned worker){auto& b=*static_cast<Batch*>(data);auto& i=*b.self;
                try {
                    VkCommandBufferInheritanceRenderingInfo formats{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};formats.colorAttachmentCount=1;formats.pColorAttachmentFormats=&b.color;formats.depthAttachmentFormat=formats.stencilAttachmentFormat=b.depth;formats.rasterizationSamples=b.samples;
                    VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};inherit.pNext=&formats;
                    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;begin.pInheritanceInfo=&inherit;
                    const auto cmd=b.commands[worker];check(i.context.api.BeginCommandBuffer(cmd,&begin),"begin worker recording");
                    Frame::Bindings bindings;
                    const unsigned start=unsigned(i.prepared.size())*worker/b.workers,end=unsigned(i.prepared.size())*(worker+1)/b.workers;
                    for(unsigned n=start;n<end;++n)recordDraw(i.context,cmd,i.prepared[n],bindings,i.config.cacheBindings);
                    check(i.context.api.EndCommandBuffer(cmd),"finish worker recording");
                }catch(...){b.errors[worker]=std::current_exception();}
            };
            config.dispatchRecording(task,&batch,workers);
            for(unsigned n=0;n<workers;++n)if(batch.errors[n])std::rethrow_exception(batch.errors[n]);
            rendering.flags=VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
            context.api.CmdBeginRendering(frame->cmd,&rendering);context.api.CmdExecuteCommands(frame->cmd,workers,batch.commands.data());frame->bindings={};
            ++timing.parallelBatches;timing.parallelDraws+=unsigned(prepared.size());
        }else{
            context.api.CmdBeginRendering(frame->cmd,&rendering);
            for(const auto& d:prepared)recordDraw(context,frame->cmd,d,frame->bindings,config.cacheBindings);
        }
        context.api.CmdEndRendering(frame->cmd);prepared.clear();renderingColor={};renderingDepth={};
        timing.commandsMs+=elapsed(started);
    }
    void barrier(Image& i, VkImageLayout layout) {
        if (i.layout == layout) {
            if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
            // Draws in one attachment scope retain native raster, depth and
            // blend ordering. A scope change still requires a memory barrier.
            if(renderingColor && ((layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && &resources.get(renderingColor).image==&i) ||
                (layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && renderingDepth && &resources.get(renderingDepth).image==&i)))return;
        }
        endRendering();
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = i.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = i.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = i.layout; b.newLayout = layout; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = i.image;
        b.subresourceRange = {i.aspect, 0, i.desc.levels, 0, i.layers};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
        context.api.CmdPipelineBarrier2(frame->cmd, &dep); i.layout = layout;
    }
    void makeCache() {
        std::vector<char> bytes;
        if (!config.cacheDirectory.empty()) {
            std::filesystem::create_directories(config.cacheDirectory);
            const auto& p = context.selected.properties; std::string key = "native-v1-" + std::to_string(p.vendorID) + "-" + std::to_string(p.deviceID) + "-" + std::to_string(p.driverVersion) + "-";
            for (unsigned char b : p.pipelineCacheUUID) { const char* hex = "0123456789abcdef"; key += hex[b >> 4]; key += hex[b & 15]; }
            cachePath = std::filesystem::path(config.cacheDirectory) / (key + ".pipeline");
            std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
            if (file && file.tellg() >= 32 && file.tellg() < 64 * 1024 * 1024) {
                bytes.resize(std::size_t(file.tellg())); file.seekg(0); file.read(bytes.data(), bytes.size());
                // Vulkan's cache header is externally supplied data. Reject a
                // corrupt or foreign device cache before handing it to a driver.
                std::uint32_t header[4]; std::memcpy(header, bytes.data(), sizeof(header));
                if (!file || header[0] != 32 || header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header[2] != p.vendorID || header[3] != p.deviceID || std::memcmp(bytes.data() + 16, p.pipelineCacheUUID, 16)) bytes.clear();
            }
        }
        VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; info.initialDataSize = bytes.size(); info.pInitialData = bytes.data();
        check(context.api.CreatePipelineCache(context.device, &info, nullptr, &cache), "create device-specific pipeline cache");
    }
    void saveCache() noexcept {
        if (cachePath.empty() || !cache) return;
        try {
            std::size_t bytes = 0; if (context.api.GetPipelineCacheData(context.device, cache, &bytes, nullptr) != VK_SUCCESS || bytes > 64 * 1024 * 1024) return;
            std::vector<char> data(bytes); if (context.api.GetPipelineCacheData(context.device, cache, &bytes, data.data()) != VK_SUCCESS) return;
            auto partial = cachePath; partial += ".partial";
            { std::ofstream out(partial, std::ios::binary | std::ios::trunc); out.write(data.data(), bytes); if (!out) return; }
            // Windows rename does not overwrite. Replace atomically on the same
            // volume, only at the acknowledged graphics lifetime barrier.
            MoveFileExW(partial.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        } catch (...) {}
    }
    void destroySwapchain() {
        for (auto s : presented) context.api.DestroySemaphore(context.device, s, nullptr);
        presented.clear(); swapImages.clear(); swapInitialized.clear();
        if (swapchain) context.api.DestroySwapchainKHR(context.device, swapchain, nullptr); swapchain = VK_NULL_HANDLE;
    }
    bool ensureSwapchain() {
        if (!context.surface) return false;
        RECT rect{}; GetClientRect(static_cast<HWND>(config.window), &rect);
        if (IsIconic(static_cast<HWND>(config.window)) || rect.right <= 0 || rect.bottom <= 0) return false;
        if (!swapDirty && swapExtent.width == unsigned(rect.right) && swapExtent.height == unsigned(rect.bottom)) return true;
        // Presentation semaphores have a different lifetime from frame resources.
        // Waiting the queue is confined to swapchain teardown/recreation; normal
        // frames use the timeline and a per-image present semaphore.
        check(context.api.QueueWaitIdle(context.queue), "swapchain recreation lifetime barrier"); destroySwapchain();
        VkSurfaceCapabilitiesKHR caps{}; check(context.api.GetPhysicalDeviceSurfaceCapabilitiesKHR(context.selected.device, context.surface, &caps), "query surface capabilities");
        unsigned n = 0; check(context.api.GetPhysicalDeviceSurfaceFormatsKHR(context.selected.device, context.surface, &n, nullptr), "query surface formats");
        std::vector<VkSurfaceFormatKHR> formats(n); check(context.api.GetPhysicalDeviceSurfaceFormatsKHR(context.selected.device, context.surface, &n, formats.data()), "query surface formats");
        auto selected = std::find_if(formats.begin(), formats.end(), [](const auto& f) { return f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; });
        if (selected == formats.end()) throw std::runtime_error("Stock UNORM presentation format unavailable"); swapFormat = selected->format;
        check(context.api.GetPhysicalDeviceSurfacePresentModesKHR(context.selected.device, context.surface, &n, nullptr), "query present modes");
        std::vector<VkPresentModeKHR> modes(n); check(context.api.GetPhysicalDeviceSurfacePresentModesKHR(context.selected.device, context.surface, &n, modes.data()), "query present modes");
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (!config.vsync) {
            if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        swapExtent = caps.currentExtent;
        if (swapExtent.width == UINT32_MAX) swapExtent = {std::clamp(unsigned(rect.right), caps.minImageExtent.width, caps.maxImageExtent.width), std::clamp(unsigned(rect.bottom), caps.minImageExtent.height, caps.maxImageExtent.height)};
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) throw std::runtime_error("Surface lacks required presentation transfer support");
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; info.surface = context.surface;
        info.minImageCount = (std::max)(caps.minImageCount, config.framesInFlight + 1); if (caps.maxImageCount) info.minImageCount = (std::min)(info.minImageCount, caps.maxImageCount);
        info.imageFormat = swapFormat; info.imageColorSpace = selected->colorSpace; info.imageExtent = swapExtent; info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; info.preTransform = caps.currentTransform;
        for (auto mode : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & mode) { info.compositeAlpha = mode; break; }
        info.presentMode = presentMode; info.clipped = VK_TRUE;
        check(context.api.CreateSwapchainKHR(context.device, &info, nullptr, &swapchain), "create native swapchain");
        check(context.api.GetSwapchainImagesKHR(context.device, swapchain, &n, nullptr), "query swapchain images"); swapImages.resize(n);
        check(context.api.GetSwapchainImagesKHR(context.device, swapchain, &n, swapImages.data()), "query swapchain images"); swapInitialized.assign(n, false); presented.resize(n);
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : presented) check(context.api.CreateSemaphore(context.device, &sem, nullptr, &s), "create per-image present semaphore");
        swapDirty = false; return true;
    }
    VkSampler sampler(const Sampler& d) {
        const auto found = std::find_if(samplers.begin(), samplers.end(), [&](const auto& s) { return s.desc == d; }); if (found != samplers.end()) return found->sampler;
        if (samplers.size() == 512) throw std::runtime_error("Bounded sampler cache exhausted");
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};info.magFilter=d.linear&&d.magLinear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;info.minFilter=d.linear&&d.minLinear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        info.mipmapMode = d.mipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = address(d.u); info.addressModeV = address(d.v); info.addressModeW = address(d.w);
        info.mipLodBias = d.lodBias; info.minLod = d.minLod; info.maxLod = d.maxLod;
        info.anisotropyEnable = d.maxAnisotropy > 1 && context.selected.features.samplerAnisotropy;
        info.maxAnisotropy = std::clamp(d.maxAnisotropy, 1.f, context.selected.properties.limits.maxSamplerAnisotropy);
        info.compareEnable = d.compare; info.compareOp = VkCompareOp(d.comparison); info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler value; check(context.api.CreateSampler(context.device, &info, nullptr, &value), "create stock sampler"); samplers.push_back({d, value}); return value;
    }
    bool shaderBias(const MaterialState& state,VkFormat depth) const {
        return depth!=VK_FORMAT_UNDEFINED&&depth!=VK_FORMAT_D24_UNORM_S8_UINT&&
            (state.depthBias||state.slopeDepthBias)&&!context.selected.depthBiasFloat&&!context.selected.depthBiasUnorm;
    }
    void rebuildPipelineIndex(){
        previousWarmPipeline=nullptr;
        pipelineIndex.fill(0);
        for(unsigned n=0;n<pipelines.size();++n){const auto& p=pipelines[n];unsigned at=pipelineHash(p.vs,p.ps,p.state,p.strides,p.elements,p.color,p.depth,p.samples,p.depthSamplerMask)&(cacheSlots-1);while(pipelineIndex[at])at=(at+1)&(cacheSlots-1);pipelineIndex[at]=n+1;}
    }
    void retireDescriptors(Frame& f){
        if(!f.descriptorsDirty)return;
        for(auto it=f.cachedDescriptors.begin();it!=f.cachedDescriptors.end();){
            bool valid=resources.find(it->vs)&&resources.find(it->ps);
            for(unsigned slot=0;valid&&slot<16;++slot)if(it->mask&(1u<<slot))valid=resources.find(it->textures[slot])!=nullptr;
            if(valid){++it;continue;}
            check(context.api.FreeDescriptorSets(context.device,f.descriptors,4,it->sets.data()),"retire completed generation-stale descriptors");it=f.cachedDescriptors.erase(it);
        }
        f.descriptorIndex.fill(0);
        for(unsigned n=0;n<f.cachedDescriptors.size();++n){const auto& d=f.cachedDescriptors[n];unsigned at=descriptorHash(d.vs,d.ps,d.textures,d.samplers,d.mask)&(cacheSlots-1);while(f.descriptorIndex[at])at=(at+1)&(cacheSlots-1);f.descriptorIndex[at]=n+1;}
        f.descriptorsDirty=false;
    }
    Pipeline* previousWarmPipeline=nullptr;
    Pipeline& pipeline(const DrawPacket& d, Image& color, Image* depth,Pipeline** previous=nullptr) {
        const auto depthFmt = depth ? depth->format : VK_FORMAT_UNDEFINED;
        unsigned shadowMask=0;for(unsigned slot=0;slot<16;++slot)if(d.samplers[slot].compare)shadowMask|=1u<<slot;
        const auto matches=[&](const Pipeline& p){return p.vs == d.vertexShader && p.ps == d.pixelShader && p.state == d.state && p.strides == d.strides && p.color == color.format && p.depth == depthFmt && p.samples == color.desc.samples && p.depthSamplerMask==shadowMask && std::equal(p.elements.begin(),p.elements.end(),d.elements.begin(),d.elements.end());};
        // Material-sorted adjacent draws usually share a full key. Check that
        // key directly before rebuilding its hash. Each preparation worker
        // owns its own previous pointer; warmed caches remain read-only.
        if(previous&&*previous&&matches(**previous))return **previous;
        unsigned cacheAt=pipelineHash(d.vertexShader,d.pixelShader,d.state,d.strides,d.elements,color.format,depthFmt,color.desc.samples,shadowMask)&(cacheSlots-1);
        while(pipelineIndex[cacheAt]){auto& p=pipelines[pipelineIndex[cacheAt]-1];if(matches(p)){if(previous)*previous=&p;return p;}cacheAt=(cacheAt+1)&(cacheSlots-1);}
        if(frame) throw std::runtime_error("Material pipeline was not warmed during asset loading");
        if (pipelines.size() >= maxDraws) throw std::runtime_error("Bounded material pipeline cache exhausted");
        if(d.state.wireframe&&!context.selected.features.fillModeNonSolid) throw std::runtime_error("Selected GPU does not support wireframe");
        if(d.state.stencil&&!depth) throw std::runtime_error("Stencil-enabled draw has no depth/stencil target");
        if (d.state.depthTest && !depth) throw std::runtime_error("Depth-enabled draw has no depth target");
        auto& vs = shader(d.vertexShader); auto& ps = shader(d.pixelShader);
        if (vs.parsed->shader_type != MOJOSHADER_TYPE_VERTEX || ps.parsed->shader_type != MOJOSHADER_TYPE_PIXEL) throw std::runtime_error("Wrong stock shader stage");
        std::vector<MOJOSHADER_vertexAttribute> attributes; std::vector<VkVertexInputAttributeDescription> inputs; std::vector<VkVertexInputBindingDescription> bindings;
        for (unsigned i = 0; i < d.elements.size(); ++i) {
            const auto& e = d.elements[i]; if (e.stream >= 16 || !d.strides[e.stream]) throw std::runtime_error("Invalid vertex declaration stream/stride");
            const MOJOSHADER_vertexElementFormat formats[]={MOJOSHADER_VERTEXELEMENTFORMAT_SINGLE,MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR2,MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR3,MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR4,MOJOSHADER_VERTEXELEMENTFORMAT_COLOR,MOJOSHADER_VERTEXELEMENTFORMAT_BYTE4,MOJOSHADER_VERTEXELEMENTFORMAT_SHORT2,MOJOSHADER_VERTEXELEMENTFORMAT_SHORT4,MOJOSHADER_VERTEXELEMENTFORMAT_NORMALIZEDSHORT2,MOJOSHADER_VERTEXELEMENTFORMAT_NORMALIZEDSHORT4,MOJOSHADER_VERTEXELEMENTFORMAT_HALFVECTOR2,MOJOSHADER_VERTEXELEMENTFORMAT_HALFVECTOR4,MOJOSHADER_VERTEXELEMENTFORMAT_COLOR,MOJOSHADER_VERTEXELEMENTFORMAT_NORMALIZEDSHORT2,MOJOSHADER_VERTEXELEMENTFORMAT_NORMALIZEDSHORT4,MOJOSHADER_VERTEXELEMENTFORMAT_BYTE4,MOJOSHADER_VERTEXELEMENTFORMAT_NORMALIZEDSHORT4};
            if(unsigned(e.format)>=std::size(formats))throw std::runtime_error("Unsupported stock vertex format");
            attributes.push_back({MOJOSHADER_usage(e.semantic),formats[unsigned(e.format)],int(e.index)});
            const bool used=std::any_of(vs.parsed->attributes,vs.parsed->attributes+vs.parsed->attribute_count,[&](const auto& a){return a.usage==MOJOSHADER_usage(e.semantic)&&a.index==int(e.index);});
            if(used) inputs.push_back({vs.inputRegister(MOJOSHADER_usage(e.semantic),e.index), e.stream, vertexFormat(e.format), e.offset});
            if (std::none_of(bindings.begin(), bindings.end(), [&](const auto& b) { return b.binding == e.stream; })) bindings.push_back({e.stream, d.strides[e.stream], VK_VERTEX_INPUT_RATE_VERTEX});
        }
        auto linked = cachedLink(config.cacheDirectory.empty() ? std::filesystem::path{} : std::filesystem::path(config.cacheDirectory) / "shaders", context, vs, ps, attributes);
        auto usedShadowMask=[&](const Shader& shader){unsigned mask=0;for(int n=0;n<shader.parsed->sampler_count;++n){const auto slot=unsigned(shader.parsed->samplers[n].index);if(slot>=16)throw std::runtime_error("Stock sampler index exceeds its bound");mask|=shadowMask&(1u<<slot);}return mask;};
        if(const auto mask=usedShadowMask(vs))linked.vertex=depthCompare(linked.vertex,mask);
        if(const auto mask=usedShadowMask(ps))linked.pixel=depthCompare(linked.pixel,mask);
        const bool biasInShader=shaderBias(d.state,depthFmt);
        if(d.state.alphaTest||biasInShader)linked.pixel=fragmentState(linked.pixel,d.state.alphaTest?int(d.state.alphaCompare):-1,biasInShader,d.state.slopeDepthBias!=0);
        Pipeline p; p.vs = d.vertexShader; p.ps = d.pixelShader; p.state = d.state; p.color = color.format; p.depth = depthFmt; p.samples = color.desc.samples; p.strides = d.strides; p.elements.assign(d.elements.begin(), d.elements.end());
        p.depthSamplerMask=shadowMask;
        VkShaderModule modules[2]{};
        try {
            for (unsigned i = 0; i < 4; ++i) {
                const auto& s = i < 2 ? vs : ps; const auto stage = i < 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
                std::vector<VkDescriptorSetLayoutBinding> entries;
                if (i % 2 == 0) for (int n = 0; n < s.parsed->sampler_count; ++n) entries.push_back({unsigned(s.parsed->samplers[n].index), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VkShaderStageFlags(stage), nullptr});
                else if (s.uniformBytes) entries.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VkShaderStageFlags(stage), nullptr});
                VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount = unsigned(entries.size()); info.pBindings = entries.data();
                check(context.api.CreateDescriptorSetLayout(context.device, &info, nullptr, &p.sets[i]), "create reflected descriptor set layout");
            }
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 4; layout.pSetLayouts = p.sets.data();
            VkPushConstantRange fragmentRange{VK_SHADER_STAGE_FRAGMENT_BIT,0,12};if(d.state.alphaTest||biasInShader){layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&fragmentRange;}
            check(context.api.CreatePipelineLayout(context.device, &layout, nullptr, &p.layout), "create stock pipeline layout");
            const std::vector<std::uint32_t>* code[] = {&linked.vertex, &linked.pixel};
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (unsigned i = 0; i < 2; ++i) {
                VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = code[i]->size() * 4; info.pCode = code[i]->data();
                check(context.api.CreateShaderModule(context.device, &info, nullptr, &modules[i]), "create translated stock shader module");
                stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module = modules[i]; stages[i].pName = "main";
            }
            VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vertex.vertexBindingDescriptionCount = unsigned(bindings.size()); vertex.pVertexBindingDescriptions = bindings.data(); vertex.vertexAttributeDescriptionCount = unsigned(inputs.size()); vertex.pVertexAttributeDescriptions = inputs.data();
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = d.state.wireframe?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL; raster.cullMode = d.state.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE; raster.frontFace = d.state.clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
            VkDepthBiasRepresentationInfoEXT bias{VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT};
            if(!biasInShader&&(d.state.depthBias||d.state.slopeDepthBias)) {
                raster.depthBiasEnable=VK_TRUE;raster.depthBiasSlopeFactor=d.state.slopeDepthBias;
                if(context.selected.depthBiasFloat) {
                    // D3D9's constant bias is already in normalized depth
                    // units. FLOAT representation keeps it independent of the
                    // native attachment format and a triangle's depth exponent.
                    bias.depthBiasRepresentation=VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;
                    bias.depthBiasExact=context.selected.depthBiasExact;
                    raster.pNext=&bias;raster.depthBiasConstantFactor=d.state.depthBias;
                } else if(context.selected.depthBiasUnorm) {
                    // D24 and D32's 23-bit mantissa plus the implicit leading
                    // bit both use r=2^-24 with exact FORCE_UNORM bias. Unlike
                    // default D32 bias, this does not vary with depth exponent.
                    bias.depthBiasRepresentation=VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
                    bias.depthBiasExact=context.selected.depthBiasExact;
                    raster.pNext=&bias;raster.depthBiasConstantFactor=d.state.depthBias*16777216.f;
                } else {
                    if(depth&&depth->format!=VK_FORMAT_D24_UNORM_S8_UINT) throw std::runtime_error("Floating-point stock depth bias requires VK_EXT_depth_bias_control float or forced UNORM representation");
                    raster.depthBiasConstantFactor=d.state.depthBias*16777216.f;
                }
            }
            VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; msaa.rasterizationSamples = VkSampleCountFlagBits(color.desc.samples);
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = d.state.depthTest; ds.depthWriteEnable = d.state.depthWrite; ds.depthCompareOp = VkCompareOp(d.state.depthCompare);
            auto stencil=[&](const MaterialState::Stencil& s){return VkStencilOpState{VkStencilOp(s.fail),VkStencilOp(s.pass),VkStencilOp(s.depthFail),VkCompareOp(s.compare),d.state.stencilReadMask,d.state.stencilWriteMask,0};};
            ds.stencilTestEnable=d.state.stencil;ds.front=stencil(d.state.front);ds.back=stencil(d.state.back);
            VkPipelineColorBlendAttachmentState attachment{}; attachment.blendEnable = d.state.blend; attachment.srcColorBlendFactor = blend(d.state.src); attachment.dstColorBlendFactor = blend(d.state.dst); attachment.colorBlendOp = VkBlendOp(d.state.blendOp); attachment.srcAlphaBlendFactor = blend(d.state.srcAlpha); attachment.dstAlphaBlendFactor = blend(d.state.dstAlpha); attachment.alphaBlendOp = VkBlendOp(d.state.alphaBlendOp); attachment.colorWriteMask = d.state.colorMask;
            VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blending.attachmentCount = 1; blending.pAttachments = &attachment;
            const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 3; dynamic.pDynamicStates = dynamicStates;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &color.format; rendering.depthAttachmentFormat = depthFmt; rendering.stencilAttachmentFormat = depthFmt;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.pNext = &rendering; info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly; info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &msaa; info.pDepthStencilState = &ds; info.pColorBlendState = &blending; info.pDynamicState = &dynamic; info.layout = p.layout;
            check(context.api.CreateGraphicsPipelines(context.device, cache, 1, &info, nullptr, &p.pipeline), "create native stock material pipeline");
        } catch (...) {
            for (auto module : modules) if (module) context.api.DestroyShaderModule(context.device, module, nullptr);
            if (p.layout) context.api.DestroyPipelineLayout(context.device, p.layout, nullptr);
            for (auto set : p.sets) if (set) context.api.DestroyDescriptorSetLayout(context.device, set, nullptr);
            throw;
        }
        for (auto module : modules) context.api.DestroyShaderModule(context.device, module, nullptr);
        pipelines.push_back(std::move(p));pipelineIndex[cacheAt]=unsigned(pipelines.size());if(previous)*previous=&pipelines.back();return pipelines.back();
    }
    void cleanup() noexcept {
        if (!context.device) return;
        context.api.DeviceWaitIdle(context.device); saveCache();
        // Also covers creation failures and resources whose engine references
        // outlived the last normal frame during an acknowledged restart.
        resources.forEach([&](Resource& r) { destroy(r); });
        for (auto& r : retired) destroy(*r.resource); retired.clear();
        for (auto& p : pipelines) destroy(p);
        for (auto& p : retiredPipelines) destroy(p);
        for (auto& s : samplers) context.api.DestroySampler(context.device, s.sampler, nullptr);
        destroySwapchain();
        for (auto& f : frames) {
            destroy(f.arena);
            if (f.pool) context.api.DestroyCommandPool(context.device, f.pool, nullptr);
            for(auto& r:f.workers)if(r.pool)context.api.DestroyCommandPool(context.device,r.pool,nullptr);
            if (f.descriptors) context.api.DestroyDescriptorPool(context.device, f.descriptors, nullptr);
            if (f.queries) context.api.DestroyQueryPool(context.device, f.queries, nullptr);
            if (f.acquired) context.api.DestroySemaphore(context.device, f.acquired, nullptr);
        }
        if (timeline) context.api.DestroySemaphore(context.device, timeline, nullptr);
        if (cache) context.api.DestroyPipelineCache(context.device, cache, nullptr);
    }
    ~Impl() { cleanup(); }
};
Backend::Backend(const gfx::Config& config) : impl(std::make_unique<Impl>(config)) {}
Backend::~Backend() = default;
ResourceId Backend::createQuery(QueryType type) {
    auto& i=*impl;i.assertOwner();const auto id=i.resources.create();auto& q=i.resources.get(id).query;q.type=type;
    try {if(type!=QueryType::Event){VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=type==QueryType::Occlusion?VK_QUERY_TYPE_OCCLUSION:VK_QUERY_TYPE_TIMESTAMP;info.queryCount=3*64;check(i.context.api.CreateQueryPool(i.context.device,&info,nullptr,&q.pool),"create stock GPU query");}}
    catch(...){i.resources.erase(id);throw;}return id;
}
void Backend::beginQuery(ResourceId id) {
    auto& i=*impl;i.requireFrame();auto& q=i.use(id).query;
    if(q.type!=QueryType::Occlusion||!q.pool||i.activeOcclusion) throw std::runtime_error("Invalid or nested stock occlusion query");
    q.selected=unsigned(i.nextSerial%3);auto& slice=q.slices[q.selected];if(slice.serial!=i.nextSerial) i.wait(slice.serial);slice={i.nextSerial,0};q.issued=false;
    i.endRendering();i.context.api.CmdResetQueryPool(i.frame->cmd,q.pool,q.selected*64,64);i.activeOcclusion=id;
}
void Backend::endQuery(ResourceId id) {
    auto& i=*impl;i.requireFrame();auto& q=i.use(id).query;
    if(q.type==QueryType::Occlusion) {if(i.activeOcclusion!=id) throw std::runtime_error("Unmatched stock occlusion query end");i.endRendering();i.activeOcclusion={};}
    else {q.selected=unsigned(i.nextSerial%3);auto& slice=q.slices[q.selected];if(q.type!=QueryType::Event&&slice.serial!=i.nextSerial) i.wait(slice.serial);slice={i.nextSerial,1};
        if(q.type==QueryType::Timestamp) {i.endRendering();i.context.api.CmdResetQueryPool(i.frame->cmd,q.pool,q.selected*64,1);i.context.api.CmdWriteTimestamp2(i.frame->cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,q.pool,q.selected*64);}}
    q.issued=true;
}
bool Backend::queryResult(ResourceId id,std::uint64_t& value) {
    impl->assertOwner();return pollQuerySerialized(id,value);
}
bool Backend::pollQuerySerialized(ResourceId id,std::uint64_t& value) {
    auto& i=*impl;const auto& q=i.resources.get(id).query;if(!q.issued) return false;
    const auto& slice=q.slices[q.selected];std::uint64_t completed=0;check(i.context.api.GetSemaphoreCounterValue(i.context.device,i.timeline,&completed),"query GPU completion");if(completed<slice.serial) return false;
    if(q.type==QueryType::Event) {value=1;return true;}
    std::array<std::uint64_t,64> samples{};
    if(slice.count) {auto result=i.context.api.GetQueryPoolResults(i.context.device,q.pool,q.selected*64,slice.count,slice.count*8,samples.data(),8,VK_QUERY_RESULT_64_BIT);if(result==VK_NOT_READY) return false;check(result,"read stock GPU query");}
    value=0;for(unsigned n=0;n<slice.count;++n) value+=samples[n];return true;
}
ResourceId Backend::createBuffer(std::span<const std::byte> data, bool index, bool dynamic) {
    auto& i = *impl; i.assertOwner(); if (data.empty()) throw std::runtime_error("Empty graphics buffer");
    const auto id = i.resources.create(); auto& r = i.resources.get(id);
    try {if(dynamic){r.dynamicBytes.assign(data.begin(),data.end());r.usedBytes=unsigned(data.size());r.revision=1;r.buffer.bytes=data.size();r.geometryUsage=index?VK_BUFFER_USAGE_INDEX_BUFFER_BIT:VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;}
        else {r.buffer = i.makeBuffer(data.size(), index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); std::memcpy(r.buffer.mapped, data.data(), data.size()); check(vmaFlushAllocation(i.context.allocator, r.buffer.allocation, 0, data.size()), "flush static geometry");} }
    catch (...) { i.destroy(r); i.resources.erase(id); throw; } return id;
}
void Backend::updateBuffer(ResourceId id,unsigned offset,std::span<const std::byte> bytes,bool discard,bool noOverwrite) {
    auto& i=*impl;i.assertOwner();auto& r=i.resources.get(id);
    if(offset>r.buffer.bytes||bytes.size()>r.buffer.bytes-offset) throw std::runtime_error("Dynamic geometry update exceeds its bound");
    if(discard&&noOverwrite)throw std::runtime_error("Invalid stock DISCARD/NOOVERWRITE combination");
    if(!r.dynamicBytes.empty()) {
        const bool append=noOverwrite&&!discard&&i.frame&&r.uploadedSerial==i.nextSerial;
        if(append&&offset<r.readEnd&&std::uint64_t(offset)+bytes.size()>r.readBegin)throw std::runtime_error("Stock NOOVERWRITE update overlaps an earlier draw");
        if(!bytes.empty())std::memcpy(r.dynamicBytes.data()+offset,bytes.data(),bytes.size());r.usedBytes=discard?unsigned(offset+bytes.size()):(std::max)(r.usedBytes,unsigned(offset+bytes.size()));++r.revision;
        if(append){
            auto& version=r.geometryVersions[i.frameIndex];
            const bool cached=r.uploadedBuffer==version.buffer.buffer;
            if(!bytes.empty()){
                std::memcpy(static_cast<std::byte*>(r.uploadedMapped)+offset,bytes.data(),bytes.size());
                i.timing.geometryCopiedBytes+=bytes.size();
            }
            if(cached){version.revision=r.revision;if(!bytes.empty())check(vmaFlushAllocation(i.context.allocator,version.buffer.allocation,offset,bytes.size()),"flush cached geometry append");}
            r.uploadedRevision=r.revision;
        }
    }
    else {if(!r.buffer.buffer) throw std::runtime_error("Update reference is not geometry");if(!bytes.empty()){i.wait(r.lastUse);std::memcpy(static_cast<std::byte*>(r.buffer.mapped)+offset,bytes.data(),bytes.size());check(vmaFlushAllocation(i.context.allocator,r.buffer.allocation,offset,bytes.size()),"flush static geometry update");}}
}
ResourceId Backend::createImage(const ImageDesc& desc) {
    auto& i = *impl; i.assertOwner();
    if (!desc.width || !desc.height || !desc.depth || !desc.levels || desc.levels > 15 || desc.samples > 16 || !(desc.samples & 31) || (desc.samples & (desc.samples - 1))) throw std::runtime_error("Invalid stock image dimensions/mips/samples");
    const auto id = i.resources.create(); auto& r = i.resources.get(id); auto& image = r.image; image.desc = desc;
    try {
        image.format = format(desc.format, desc.srgb, i.context); image.layers = desc.kind == ImageKind::Cube ? 6 : 1;
        if (desc.format == ImageFormat::DepthStencil) image.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = desc.kind == ImageKind::Volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D; info.flags = desc.kind == ImageKind::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        info.format = image.format; info.extent = {desc.width, desc.height, desc.depth}; info.mipLevels = desc.levels; info.arrayLayers = image.layers;
        info.samples = VkSampleCountFlagBits(desc.samples); info.tiling = VK_IMAGE_TILING_OPTIMAL; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (desc.sampled) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (desc.renderTarget) info.usage |= desc.format == ImageFormat::DepthStencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VmaAllocationCreateInfo alloc{}; alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        check(vmaCreateImage(i.context.allocator, &info, &alloc, &image.image, &image.allocation, nullptr), "allocate native stock image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = image.image; view.viewType = desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : desc.kind == ImageKind::Volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
        // Attachments use both depth and stencil. Descriptors must use a
        // separate depth-only view of the same generation/lifetime.
        view.subresourceRange = {image.aspect, 0, desc.levels, 0, image.layers};
        if(desc.format==ImageFormat::R8) view.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
        if(desc.format==ImageFormat::Rg8) view.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G};
        if(desc.format==ImageFormat::Alpha8) view.components={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};
        check(i.context.api.CreateImageView(i.context.device, &view, nullptr, &image.view), "create stock image view");
        if(desc.sampled&&desc.format==ImageFormat::DepthStencil){view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;check(i.context.api.CreateImageView(i.context.device,&view,nullptr,&image.sampledView),"create sampled stock depth view");}
        if(desc.sampled&&desc.format==ImageFormat::R32Float){
            // D3D9 R32F sampling returns (r,1,1,1), unlike Vulkan's
            // (r,0,0,1). Keep the attachment view unswizzled; only the sampled
            // view supplies the defaults used by stock soft-particle shaders.
            view.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE,VK_COMPONENT_SWIZZLE_ONE,VK_COMPONENT_SWIZZLE_ONE};
            check(i.context.api.CreateImageView(i.context.device,&view,nullptr,&image.sampledView),"create stock R32F sampled view");
        }
    } catch (...) { i.destroy(r); i.resources.erase(id); throw; } return id;
}
void Backend::uploadImage(ResourceId id, unsigned level, unsigned layer, std::span<const std::byte> data) {
    auto& i = *impl; i.requireFrame(); auto& image = i.image(id);
    if (level >= image.desc.levels || layer >= image.layers || image.desc.samples != 1 || data.size() != imageBytes(image.desc, level)) throw std::runtime_error("Invalid stock texture upload extent");
    const auto offset = i.allocate(data.size(), 16); std::memcpy(static_cast<std::byte*>(i.frame->arena.mapped) + offset, data.data(), data.size());
    i.barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{}; copy.bufferOffset = offset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}; copy.imageExtent = {(std::max)(1u, image.desc.width >> level), (std::max)(1u, image.desc.height >> level), (std::max)(1u, image.desc.depth >> level)};
    i.context.api.CmdCopyBufferToImage(i.frame->cmd, i.frame->arena.buffer, image.image, image.layout, 1, &copy);
}
ResourceId Backend::createShader(std::span<const std::uint32_t> bytecode, std::string name) {
    auto& i = *impl; i.assertOwner(); auto shader = std::make_unique<Shader>(bytecode, std::move(name)); const auto id = i.resources.create(); i.resources.get(id).shader = std::move(shader); return id;
}
void Backend::release(ResourceId id) {
    auto& i = *impl; i.assertOwner(); auto& resource = i.resources.get(id);
    if(resource.image.image||resource.shader)for(auto& f:i.frames)f.descriptorsDirty=true;
    if (i.retired.size() == maxResources) { i.collect(); if (i.retired.size() == maxResources) throw std::runtime_error("Bounded resource retirement queue exhausted"); }
    const auto serial = resource.lastUse;
    if(resource.shader) {for(auto it=i.pipelines.begin();it!=i.pipelines.end();) {
        if(it->vs==id||it->ps==id) {
            if(i.retiredPipelines.size()==maxDraws) {i.collect();if(i.retiredPipelines.size()==maxDraws) throw std::runtime_error("Bounded pipeline retirement queue exhausted");}
            i.retiredPipelines.push_back(std::move(*it));it=i.pipelines.erase(it);
        }else ++it;
    }i.rebuildPipelineIndex();}
    i.retired.push_back({serial, i.resources.extract(id)});
}
void Backend::beginFrame() {
    auto& i = *impl; i.assertOwner(); if(i.recordingFailed)throw std::runtime_error("Vulkan recording failed; a renderer restart is required");if (i.frame) throw std::runtime_error("Nested Vulkan frame");
    i.frame = &i.frames[i.frameIndex]; auto& f = *i.frame; i.timing = {}; const auto start = Clock::now(); i.wait(f.serial); i.timing.recycleWaitMs = elapsed(start);
    if (f.serial) {
        std::uint64_t stamps[2]; check(i.context.api.GetQueryPoolResults(i.context.device, f.queries, 0, 2, sizeof(stamps), stamps, 8, VK_QUERY_RESULT_64_BIT), "read completed GPU timestamps");
        i.timing.gpuMs = double(stamps[1] - stamps[0]) * i.context.selected.properties.limits.timestampPeriod / 1000000.;
        i.timing.gpuSerial=f.serial;
    }
    i.retireDescriptors(f);i.collect(); check(i.context.api.ResetCommandPool(i.context.device, f.pool, 0), "recycle completed command pool");
    if(!i.config.cacheDescriptors){check(i.context.api.ResetDescriptorPool(i.context.device,f.descriptors,0),"recycle completed descriptors");f.cachedDescriptors.clear();f.descriptorIndex.fill(0);}
    for(auto& r:f.workers)if(r.pool){check(i.context.api.ResetCommandPool(i.context.device,r.pool,0),"recycle completed worker pool");r.used=0;}
    i.plans.clear();i.uniformJobs.clear();i.planCursor=0;
    i.previousDescriptorVersion=0;i.previousDescriptors=nullptr;
    f.used = 0; f.draws = 0;f.bindings={}; ++i.nextSerial; i.timing.serial = i.nextSerial; i.recording = Clock::now();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(i.context.api.BeginCommandBuffer(f.cmd, &begin), "begin native frame"); i.context.api.CmdResetQueryPool(f.cmd, f.queries, 0, 2); i.context.api.CmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, f.queries, 0);
}
void Backend::clear(ResourceId color, ResourceId depth, const std::array<float, 4>& rgba, float depthValue,
    unsigned stencilValue,bool clearDepth,bool clearStencil,std::span<const ClearRect> rectangles) {
    auto& i = *impl; i.requireFrame();i.endRendering();
    if (!clearDepth&&!clearStencil) depth={};
    if (rectangles.size()>256) throw std::runtime_error("Bounded stock clear rectangle count exceeded");
    if(!rectangles.empty()) {
        auto* c=color?&i.image(color):nullptr;auto* d=depth?&i.image(depth):nullptr;
        if(!c&&!d)return;const auto& desc=c?c->desc:d->desc;
        if(c&&d&&(c->desc.width!=d->desc.width||c->desc.height!=d->desc.height||c->desc.samples!=d->desc.samples))throw std::runtime_error("Mismatched partial clear targets");
        std::array<VkClearRect,256> rects{};unsigned count=0;
        for(const auto& r:rectangles) {
            if(r.right<r.left||r.bottom<r.top)throw std::runtime_error("Invalid stock clear rectangle");
            const auto left=std::clamp(r.left,0,int(desc.width)),top=std::clamp(r.top,0,int(desc.height));
            const auto right=std::clamp(r.right,left,int(desc.width)),bottom=std::clamp(r.bottom,top,int(desc.height));
            if(right>left&&bottom>top)rects[count++]={{{left,top},{unsigned(right-left),unsigned(bottom-top)}},0,1};
        }
        if(!count)return;
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},ds{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if(c){i.barrier(*c,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);ca.imageView=c->view;ca.imageLayout=c->layout;ca.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
        if(d){i.barrier(*d,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);ds.imageView=d->view;ds.imageLayout=d->layout;ds.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;ds.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};rendering.renderArea.extent={desc.width,desc.height};rendering.layerCount=1;
        rendering.colorAttachmentCount=c?1:0;rendering.pColorAttachments=c?&ca:nullptr;
        rendering.pDepthAttachment=d&&clearDepth?&ds:nullptr;rendering.pStencilAttachment=d&&clearStencil?&ds:nullptr;
        std::array<VkClearAttachment,2> attachments{};unsigned used=0;
        if(c){auto& a=attachments[used++];a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;std::memcpy(a.clearValue.color.float32,rgba.data(),16);}
        if(d){auto& a=attachments[used++];a.aspectMask=(clearDepth?VK_IMAGE_ASPECT_DEPTH_BIT:0)|(clearStencil?VK_IMAGE_ASPECT_STENCIL_BIT:0);a.clearValue.depthStencil={depthValue,stencilValue};}
        i.context.api.CmdBeginRendering(i.frame->cmd,&rendering);i.context.api.CmdClearAttachments(i.frame->cmd,used,attachments.data(),count,rects.data());i.context.api.CmdEndRendering(i.frame->cmd);return;
    }
    if (color) { auto& image = i.image(color); i.barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); VkClearColorValue value{}; std::memcpy(value.float32, rgba.data(), 16); VkImageSubresourceRange range{image.aspect, 0, 1, 0, 1}; i.context.api.CmdClearColorImage(i.frame->cmd, image.image, image.layout, &value, 1, &range); }
    if (depth) { auto& image = i.image(depth); i.barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); VkClearDepthStencilValue value{depthValue, stencilValue}; VkImageSubresourceRange range{VkImageAspectFlags((clearDepth?VK_IMAGE_ASPECT_DEPTH_BIT:0)|(clearStencil?VK_IMAGE_ASPECT_STENCIL_BIT:0)), 0, 1, 0, 1}; i.context.api.CmdClearDepthStencilImage(i.frame->cmd, image.image, image.layout, &value, 1, &range); }
}
void Backend::draw(const DrawPacket& d) {
    auto& i = *impl; i.requireFrame(); if (i.frame->draws >= maxDraws) throw std::runtime_error("Bounded frame draw budget exhausted");
    if(i.renderingColor && (i.renderingColor!=d.colorTarget || i.renderingDepth!=d.depthTarget))i.endRendering();
    const Impl::DrawPlan* plan=i.planCursor<i.plans.size()&&i.plans[i.planCursor].source==&d?&i.plans[i.planCursor++]:nullptr;
    auto& color = plan?*plan->color:i.image(d.colorTarget); auto* depth = plan?plan->depth:(d.depthTarget ? &i.image(d.depthTarget) : nullptr);
    auto& vs = plan?*plan->vertex:i.shader(d.vertexShader); auto& ps = plan?*plan->pixel:i.shader(d.pixelShader);
    auto& pipeline = plan?*plan->pipeline:i.pipeline(d, color, depth);
    pipeline.lastUse=i.nextSerial;
    Frame::Descriptors* descriptors=d.immutableDescriptorVersion&&d.immutableDescriptorVersion==i.previousDescriptorVersion?i.previousDescriptors:nullptr;
    unsigned samplerMask=plan?plan->samplerMask:0;
    if(!plan)for(const auto* shader:{&vs,&ps})for(int n=0;n<shader->parsed->sampler_count;++n)samplerMask|=1u<<shader->parsed->samplers[n].index;
    unsigned descriptorAt=(plan?plan->descriptorHash:descriptorHash(d.vertexShader,d.pixelShader,d.textures,d.samplers,samplerMask))&(cacheSlots-1);
    while(!descriptors&&i.frame->descriptorIndex[descriptorAt]) {
        auto& cached=i.frame->cachedDescriptors[i.frame->descriptorIndex[descriptorAt]-1];
        bool match=cached.vs==d.vertexShader&&cached.ps==d.pixelShader;
        for(unsigned slot=0;match&&slot<16;++slot)if(samplerMask&(1u<<slot))if(cached.textures[slot]!=d.textures[slot]||cached.samplers[slot]!=d.samplers[slot]){match=false;break;}
        if(match){descriptors=&cached;break;}
        descriptorAt=(descriptorAt+1)&(cacheSlots-1);
    }
    const bool writeDescriptors=!descriptors;
    if(writeDescriptors) {
        if(i.frame->cachedDescriptors.size()==maxDraws) throw std::runtime_error("Bounded frame descriptor cache exhausted");
        // Layouts for one shader pair are structurally identical across
        // material pipelines. Ignore unused stock bindings and reuse their
        // compatible sets instead of allocating four sets per state variant.
        i.frame->cachedDescriptors.push_back({d.vertexShader,d.pixelShader,samplerMask,d.textures,d.samplers,{}});descriptors=&i.frame->cachedDescriptors.back();
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=i.frame->descriptors;alloc.descriptorSetCount=4;alloc.pSetLayouts=pipeline.sets.data();
        check(i.context.api.AllocateDescriptorSets(i.context.device,&alloc,descriptors->sets.data()),"allocate bounded draw descriptors");
        i.frame->descriptorIndex[descriptorAt]=unsigned(i.frame->cachedDescriptors.size());
    }
    i.previousDescriptorVersion=d.immutableDescriptorVersion;i.previousDescriptors=descriptors;
    const auto& sets=descriptors->sets;
    // Initialize only descriptors actually written. Cached draws need no
    // descriptor updates and must not clear several kilobytes of scratch data.
    std::array<VkWriteDescriptorSet,34> writes;unsigned writeCount=0,imageCount=0;
    std::array<VkDescriptorImageInfo,32> images;
    std::array<VkDescriptorBufferInfo,2> buffers;
    std::array<unsigned, 2> offsets{}; unsigned offsetCount = 0;
    for (unsigned stage = 0; stage < 2; ++stage) {
        const auto& shader = stage ? ps : vs;
        for (int n = 0; n < shader.parsed->sampler_count; ++n) {
            const unsigned slot = shader.parsed->samplers[n].index; if (slot >= 16 || !d.textures[slot]) throw std::runtime_error("Missing reflected stock texture binding");
            auto& tex = i.image(d.textures[slot]); if (&tex == &color || &tex == depth) throw std::runtime_error("Render-target feedback loop is unsupported");
            i.barrier(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if(writeDescriptors) {
                if(d.samplers[slot].compare&&tex.desc.format!=ImageFormat::DepthStencil)throw std::runtime_error("Shadow comparison requires a depth texture");
                auto& image=images[imageCount++];image={i.sampler(d.samplers[slot]), tex.sampledView?tex.sampledView:tex.view, tex.layout};
                auto& write=writes[writeCount++];write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=sets[stage*2];write.dstBinding=slot;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
            }
        }
        if (shader.uniformBytes) {
            if (shader.uniformBytes > i.context.selected.properties.limits.maxUniformBufferRange) throw std::runtime_error("Stock uniforms exceed GPU range");
            const auto id=stage?d.pixelShader:d.vertexShader;
            const auto floats=stage?d.pixelConstants:d.vertexConstants;const auto integers=stage?d.pixelIntegers:d.vertexIntegers,booleans=stage?d.pixelBooleans:d.vertexBooleans;
            auto& cached=i.frame->uniforms[(id.value&65535u)-1];
            const bool immutable=i.config.cacheBindings&&d.immutableConstants;
            unsigned offset=cached.offset;
            if(!plan&&(!immutable||cached.zeroTail!=d.zeroConstantTail||cached.serial!=i.nextSerial||cached.shader!=id||cached.floats.data()!=floats.data()||cached.floats.size()!=floats.size()||cached.integers.data()!=integers.data()||cached.integers.size()!=integers.size()||cached.booleans.data()!=booleans.data()||cached.booleans.size()!=booleans.size())){
                offset=unsigned(i.allocate(shader.uniformBytes,(std::max)(VkDeviceSize(16),i.context.selected.properties.limits.minUniformBufferOffsetAlignment)));
                shader.packUniforms(static_cast<std::byte*>(i.frame->arena.mapped)+offset,floats,integers,booleans,d.zeroConstantTail);
                cached={id,immutable?i.nextSerial:0,floats,integers,booleans,offset,d.zeroConstantTail};
            }
            offsets[offsetCount] = plan?plan->packet.offsets[offsetCount]:offset;++offsetCount;
            if(writeDescriptors) {auto& buffer=buffers[stage];buffer={i.frame->arena.buffer,0,shader.uniformBytes};auto& write=writes[writeCount++];write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=sets[stage*2+1];write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;write.pBufferInfo=&buffer;}
        }
    }
    if(writeCount)i.context.api.UpdateDescriptorSets(i.context.device,writeCount,writes.data(),0,nullptr);
    i.barrier(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) {
        // Stock reduced-resolution passes retain the larger scene depth
        // surface. Vulkan allows it when every attachment covers renderArea.
        if (depth->desc.samples != color.desc.samples || depth->desc.width < color.desc.width || depth->desc.height < color.desc.height) throw std::runtime_error("Stock depth target does not cover its render target or has different samples");
        i.barrier(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
    Resource::Query* occlusion=nullptr; unsigned queryIndex=0;
    if(i.activeOcclusion) {
        occlusion=&i.use(i.activeOcclusion).query;auto& slice=occlusion->slices[occlusion->selected];
        if(slice.count>=64) throw std::runtime_error("Bounded occlusion draw count exhausted");
        queryIndex=occlusion->selected*64+slice.count++;
    }
    // Validate and allocate geometry before opening the rendering scope. A
    // bounded-arena or draw-range failure must never leave rendering open.
    if(d.state.alphaTest&&d.state.alphaRef>255)throw std::runtime_error("Invalid stock alpha reference");
    std::array<VkBuffer,16> vertexBuffers{};std::array<VkDeviceSize,16> vertexOffsets{};
    for(unsigned s=0;s<16;++s)if(d.vertexBuffers[s]){
        auto& r=i.use(d.vertexBuffers[s]);auto& b=r.buffer;if(d.vertexOffsets[s]>=b.bytes)throw std::runtime_error("Invalid stock vertex buffer/offset");
        const auto [buffer,base]=i.geometry(r);vertexBuffers[s]=buffer;vertexOffsets[s]=base+d.vertexOffsets[s];
        if(!r.dynamicBytes.empty()){const auto first=std::int64_t(d.vertexBase)+d.vertexFirst;if(first<0)throw std::runtime_error("Invalid stock dynamic vertex range");const auto begin=std::uint64_t(d.vertexOffsets[s])+std::uint64_t(first)*d.strides[s],end=d.vertexCount?begin+std::uint64_t(d.vertexCount)*d.strides[s]:r.usedBytes;if(end>b.bytes||end<begin)throw std::runtime_error("Stock dynamic vertex range exceeds buffer");r.readBegin=(std::min)(r.readBegin,begin);r.readEnd=(std::max)(r.readEnd,end);}
    }
    auto& indexResource=i.use(d.indexBuffer);const auto indexWidth=d.index32?4u:2u;
    if((std::uint64_t(d.firstIndex)+d.indexCount)*indexWidth>indexResource.buffer.bytes)throw std::runtime_error("Invalid stock draw index range");
    const auto [indexBuffer,indexOffset]=i.geometry(indexResource);
    if(!indexResource.dynamicBytes.empty()){indexResource.readBegin=(std::min)(indexResource.readBegin,std::uint64_t(d.firstIndex)*indexWidth);indexResource.readEnd=(std::max)(indexResource.readEnd,(std::uint64_t(d.firstIndex)+d.indexCount)*indexWidth);}
    if(!i.renderingColor){i.renderingColor=d.colorTarget;i.renderingDepth=d.depthTarget;}
    PreparedDraw packet=plan?plan->packet:PreparedDraw{};packet.pipeline=pipeline.pipeline;packet.layout=pipeline.layout;packet.sets=sets;packet.offsets=offsets;packet.offsetCount=offsetCount;
    packet.vertices=vertexBuffers;packet.vertexOffsets=vertexOffsets;packet.index=indexBuffer;packet.indexOffset=indexOffset;packet.indexType=d.index32?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16;
    // Negative viewport and half-pixel offset preserve stock D3D orientation.
    if(!plan)rasterState(packet,d,color.desc.width,color.desc.height,i.shaderBias(d.state,pipeline.depth));
    if(occlusion){packet.query=occlusion->pool;packet.queryIndex=queryIndex;packet.queryFlags=i.context.selected.features.occlusionQueryPrecise?VK_QUERY_CONTROL_PRECISE_BIT:0;}
    // Only contiguous triangle lists with exactly the same GPU inputs merge.
    // Query boundaries and every material/geometry/constant change stay distinct.
    if(i.config.batchDraws&&i.config.cacheBindings&&!i.prepared.empty()&&adjacentDraws(i.prepared.back(),packet)){
        i.prepared.back().indexCount+=packet.indexCount;++i.timing.batchedDraws;
    }else i.prepared.push_back(packet);
    ++i.frame->draws;
}
void Backend::prepareDraws(std::span<const DrawPacket* const> draws) {
    auto& i=*impl;i.requireFrame();
    if(!i.config.parallelPrepare||draws.empty())return;
    if(i.frame->draws||!i.plans.empty()||draws.size()>maxDraws)throw std::runtime_error("Invalid immutable draw preparation phase");
    for(const auto* source:draws){
        if(!source||!source->immutableConstants)throw std::runtime_error("Parallel preparation requires immutable draw packets");
        const auto& d=*source;auto& p=i.plans.emplace_back();p.source=source;
        p.vertex=&i.shader(d.vertexShader);p.pixel=&i.shader(d.pixelShader);
        p.color=&i.image(d.colorTarget);p.depth=d.depthTarget?&i.image(d.depthTarget):nullptr;
        for(unsigned stage=0;stage<2;++stage){
            const auto& shader=stage?*p.pixel:*p.vertex;if(!shader.uniformBytes)continue;
            if(shader.uniformBytes>i.context.selected.properties.limits.maxUniformBufferRange)throw std::runtime_error("Stock uniforms exceed GPU range");
            const auto id=stage?d.pixelShader:d.vertexShader;
            const auto floats=stage?d.pixelConstants:d.vertexConstants;
            const auto integers=stage?d.pixelIntegers:d.vertexIntegers,booleans=stage?d.pixelBooleans:d.vertexBooleans;
            auto& cached=i.frame->uniforms[(id.value&65535u)-1];
            if(!i.config.cacheBindings||cached.serial!=i.nextSerial||cached.shader!=id||cached.zeroTail!=d.zeroConstantTail||
                cached.floats.data()!=floats.data()||cached.floats.size()!=floats.size()||cached.integers.data()!=integers.data()||cached.integers.size()!=integers.size()||cached.booleans.data()!=booleans.data()||cached.booleans.size()!=booleans.size()){
                const auto offset=unsigned(i.allocate(shader.uniformBytes,(std::max)(VkDeviceSize(16),i.context.selected.properties.limits.minUniformBufferOffsetAlignment)));
                i.uniformJobs.push_back({&shader,offset,floats,integers,booleans,d.zeroConstantTail});
                cached={id,i.nextSerial,floats,integers,booleans,offset,d.zeroConstantTail};
            }
            p.packet.offsets[p.packet.offsetCount++]=cached.offset;
        }
    }
    const unsigned workers=i.config.dispatchRecording?(std::max)(1u,(std::min)(i.config.recordingWorkers,unsigned(draws.size()/64))):1;
    struct Batch {Impl* self;unsigned workers;std::array<std::exception_ptr,4> errors{};} batch{&i,workers};
    const auto task=[](void* data,unsigned worker){auto& b=*static_cast<Batch*>(data);auto& i=*b.self;
        try{
            // All mutable caches, resources and arena reservations are owned by
            // the submitting thread. Workers read warmed pipeline keys and write
            // disjoint packets/uniform slices; they make no command-pool calls.
            Pipeline* previous=nullptr;const Impl::DrawPlan* previousPlan=nullptr;
            for(std::size_t n=i.plans.size()*worker/b.workers;n<i.plans.size()*(worker+1)/b.workers;++n){
                auto& p=i.plans[n];const auto& d=*p.source;
                if(previousPlan&&d.immutablePipelineVersion&&d.immutablePipelineVersion==previousPlan->source->immutablePipelineVersion)p.pipeline=previousPlan->pipeline;
                else p.pipeline=&i.pipeline(d,*p.color,p.depth,&previous);
                const bool sameDescriptorVersion=previousPlan&&d.immutableDescriptorVersion&&d.immutableDescriptorVersion==previousPlan->source->immutableDescriptorVersion;
                bool sameDescriptors=previousPlan&&p.vertex==previousPlan->vertex&&p.pixel==previousPlan->pixel;
                if(sameDescriptors)p.samplerMask=previousPlan->samplerMask;
                else for(const auto* shader:{p.vertex,p.pixel})for(int s=0;s<shader->parsed->sampler_count;++s){const auto slot=unsigned(shader->parsed->samplers[s].index);if(slot>=16)throw std::runtime_error("Stock sampler index exceeds its bound");p.samplerMask|=1u<<slot;}
                if(sameDescriptors&&!sameDescriptorVersion)for(unsigned slot=0;slot<16;++slot)if(p.samplerMask&(1u<<slot)){
                    const auto& old=*previousPlan->source;if(d.textures[slot]!=old.textures[slot]||d.samplers[slot]!=old.samplers[slot]){sameDescriptors=false;break;}}
                p.descriptorHash=sameDescriptors?previousPlan->descriptorHash:descriptorHash(d.vertexShader,d.pixelShader,d.textures,d.samplers,p.samplerMask);
                rasterState(p.packet,d,p.color->desc.width,p.color->desc.height,i.shaderBias(d.state,p.pipeline->depth));
                previousPlan=&p;
            }
            for(std::size_t n=i.uniformJobs.size()*worker/b.workers;n<i.uniformJobs.size()*(worker+1)/b.workers;++n){const auto& job=i.uniformJobs[n];job.shader->packUniforms(static_cast<std::byte*>(i.frame->arena.mapped)+job.offset,job.floats,job.integers,job.booleans,job.zeroTail);}
        }catch(...){b.errors[worker]=std::current_exception();}
    };
    if(workers>1){i.config.dispatchRecording(task,&batch,workers);++i.timing.parallelPrepareBatches;i.timing.parallelPrepareDraws+=unsigned(draws.size());}else task(&batch,0);
    for(unsigned n=0;n<workers;++n)if(batch.errors[n])std::rethrow_exception(batch.errors[n]);
}
void Backend::warm(const DrawPacket& d) {
    auto& i=*impl;i.assertOwner();if(i.frame) throw std::runtime_error("Pipeline warmup must run outside gameplay frame recording");
    auto& color=i.image(d.colorTarget);auto* depth=d.depthTarget?&i.image(d.depthTarget):nullptr;i.pipeline(d,color,depth,&i.previousWarmPipeline);
}
void Backend::resolve(ResourceId source,ResourceId destination) {
    auto& i=*impl;i.requireFrame();auto& src=i.image(source);auto& dst=i.image(destination);
    if(src.desc.samples<=1||dst.desc.samples!=1||src.format!=dst.format||src.desc.width!=dst.desc.width||src.desc.height!=dst.desc.height||src.desc.depth!=1||dst.desc.depth!=1||src.aspect!=VK_IMAGE_ASPECT_COLOR_BIT||dst.aspect!=VK_IMAGE_ASPECT_COLOR_BIT) throw std::runtime_error("Invalid stock MSAA resolve targets");
    i.barrier(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);i.barrier(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent={src.desc.width,src.desc.height,1};i.context.api.CmdResolveImage(i.frame->cmd,src.image,src.layout,dst.image,dst.layout,1,&region);
}
void Backend::blit(ResourceId source,ResourceId destination,bool linear,const ClearRect* sourceRect,const ClearRect* destinationRect) {
    auto& i=*impl;i.requireFrame();auto& src=i.image(source);auto& dst=i.image(destination);
    if(source==destination||src.aspect!=VK_IMAGE_ASPECT_COLOR_BIT||dst.aspect!=VK_IMAGE_ASPECT_COLOR_BIT||src.desc.depth!=1||dst.desc.depth!=1||dst.desc.samples!=1)throw std::runtime_error("Unsupported stock image blit");
    if(src.desc.samples!=1){if(sourceRect||destinationRect)throw std::runtime_error("Partial stock MSAA resolve is unsupported");resolve(source,destination);return;}
    const auto s=sourceRect?*sourceRect:ClearRect{0,0,int(src.desc.width),int(src.desc.height)};
    const auto d=destinationRect?*destinationRect:ClearRect{0,0,int(dst.desc.width),int(dst.desc.height)};
    const auto valid=[](const ClearRect& r,const ImageDesc& desc){return r.left>=0&&r.top>=0&&r.right>r.left&&r.bottom>r.top&&r.right<=int(desc.width)&&r.bottom<=int(desc.height);};
    if(!valid(s,src.desc)||!valid(d,dst.desc))throw std::runtime_error("Invalid stock image blit rectangle");
    const bool copy=src.format==dst.format&&s.right-s.left==d.right-d.left&&s.bottom-s.top==d.bottom-d.top;
    if(!copy){VkFormatProperties sp{},dp{};i.context.api.GetPhysicalDeviceFormatProperties(i.context.selected.device,src.format,&sp);i.context.api.GetPhysicalDeviceFormatProperties(i.context.selected.device,dst.format,&dp);
        if(!(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(dp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)||(linear&&!(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))throw std::runtime_error("Selected GPU does not support requested stock image blit");}
    i.barrier(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);i.barrier(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if(copy){VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.srcOffset={s.left,s.top,0};region.dstOffset={d.left,d.top,0};region.extent={unsigned(s.right-s.left),unsigned(s.bottom-s.top),1};i.context.api.CmdCopyImage(i.frame->cmd,src.image,src.layout,dst.image,dst.layout,1,&region);}
    else{VkImageBlit region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.srcOffsets[0]={s.left,s.top,0};region.srcOffsets[1]={s.right,s.bottom,1};region.dstOffsets[0]={d.left,d.top,0};region.dstOffsets[1]={d.right,d.bottom,1};i.context.api.CmdBlitImage(i.frame->cmd,src.image,src.layout,dst.image,dst.layout,1,&region,linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);}
}
FrameTiming Backend::endFrame(ResourceId source) {
    auto& i = *impl; i.requireFrame(); auto& f = *i.frame;
    if(i.activeOcclusion) throw std::runtime_error("Stock occlusion query spans a submission boundary");
    i.endRendering();
    bool present = false; unsigned index = 0;
    const auto swapStart=Clock::now();const bool ready=source&&i.ensureSwapchain();i.timing.swapchainMs=elapsed(swapStart);
    if (ready) {
        const auto start = Clock::now(); const auto result = i.context.api.AcquireNextImageKHR(i.context.device, i.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index); i.timing.acquireMs = elapsed(start);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) i.swapDirty = true;
        else {
            if (result == VK_SUBOPTIMAL_KHR) i.swapDirty = true; else check(result, "acquire swapchain image");
            auto& image = i.image(source); if (image.desc.samples != 1 || image.format != i.swapFormat) throw std::runtime_error("Presentation requires resolved stock BGRA UNORM image");
            i.barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.oldLayout = i.swapInitialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = i.swapImages[index]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            // Chain the layout transition after the acquire semaphore's transfer
            // wait. NONE here permits transition before presentation releases
            // the image even though the later blit waits correctly.
            b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b; i.context.api.CmdPipelineBarrier2(f.cmd, &dep);
            if(image.desc.width==i.swapExtent.width&&image.desc.height==i.swapExtent.height){
                VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={image.desc.width,image.desc.height,1};
                i.context.api.CmdCopyImage(f.cmd,image.image,image.layout,i.swapImages[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            }else{
                VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; blit.srcOffsets[1] = {int(image.desc.width), int(image.desc.height), 1}; blit.dstOffsets[1] = {int(i.swapExtent.width), int(i.swapExtent.height), 1};
                i.context.api.CmdBlitImage(f.cmd, image.image, image.layout, i.swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            }
            b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_NONE; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; i.context.api.CmdPipelineBarrier2(f.cmd, &dep); i.swapInitialized[index] = true; present = true;
        }
    }
    i.context.api.CmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.queries, 1); check(i.context.api.EndCommandBuffer(f.cmd), "finish native frame");
    check(vmaFlushAllocation(i.context.allocator, f.arena.allocation, 0, f.used), "flush frame upload arena");
    i.timing.recordMs = (std::max)(0.,elapsed(i.recording)-i.timing.acquireMs-i.timing.swapchainMs);
    // Measure CPU replay/preparation once per frame. Per-draw clock reads add
    // thousands of unnecessary timer calls to normal high-rate rendering.
    // This residual also includes uploads, queries, barriers and frame finish.
    i.timing.prepareMs=(std::max)(0.,i.timing.recordMs-i.timing.commandsMs);
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = f.cmd;
    VkSemaphoreSubmitInfo signals[2]{}; signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO; signals[0].semaphore = i.timeline; signals[0].value = i.nextSerial; signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO; signals[1].semaphore = present ? i.presented[index] : VK_NULL_HANDLE; signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo acquire{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; acquire.semaphore = f.acquired; acquire.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &command; submit.signalSemaphoreInfoCount = present ? 2 : 1; submit.pSignalSemaphoreInfos = signals; submit.waitSemaphoreInfoCount = present ? 1 : 0; submit.pWaitSemaphoreInfos = &acquire;
    auto start = Clock::now(); check(i.context.api.QueueSubmit2(i.context.queue, 1, &submit, VK_NULL_HANDLE), "submit native frame"); i.timing.submitMs = elapsed(start); f.serial = i.nextSerial;i.lastSubmitted=i.nextSerial;
    if (present) {
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; info.waitSemaphoreCount = 1; info.pWaitSemaphores = &i.presented[index]; info.swapchainCount = 1; info.pSwapchains = &i.swapchain; info.pImageIndices = &index;
        start = Clock::now(); const auto result = i.context.api.QueuePresentKHR(i.context.queue, &info); i.timing.presentMs = elapsed(start);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) i.swapDirty = true; else check(result, "present native frame");
    }
    i.timing.draws = f.draws; i.frame = nullptr; i.frameIndex = (i.frameIndex + 1) % i.config.framesInFlight; return i.timing;
}
std::vector<std::byte> Backend::readback(ResourceId id) {
    auto& i = *impl; i.assertOwner(); if (i.frame) throw std::runtime_error("Readback requires an ended frame");
    const auto& desc = i.resources.get(id).image.desc; if (desc.samples != 1) throw std::runtime_error("Readback requires resolved pixels");
    const auto bytes = imageBytes(desc, 0); auto buffer = i.makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    try {
        beginFrame(); auto& image = i.image(id); i.barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {desc.width, desc.height, desc.depth};
        i.context.api.CmdCopyImageToBuffer(i.frame->cmd, image.image, image.layout, buffer.buffer, 1, &copy); const auto serial = endFrame({}).serial; i.wait(serial);
        check(vmaInvalidateAllocation(i.context.allocator, buffer.allocation, 0, bytes), "invalidate GPU readback");
        std::vector<std::byte> result(bytes); std::memcpy(result.data(), buffer.mapped, bytes); i.destroy(buffer); return result;
    } catch (...) { i.destroy(buffer); throw; }
}
void Backend::waitIdle() { auto& i = *impl; i.assertOwner(); if (i.frame) throw std::runtime_error("Idle barrier inside a frame"); i.wait(i.lastSubmitted); i.collect(); }
FrameTiming Backend::latestTiming() const {impl->assertOwner();return impl->timing;}
std::array<Backend::CompletedGpuTiming,3> Backend::completeFrameTimings(){
    auto& i=*impl;i.assertOwner();waitIdle();std::array<CompletedGpuTiming,3> result{};
    for(unsigned n=0;n<i.frames.size();++n){const auto& f=i.frames[n];if(!f.serial)continue;
        std::uint64_t stamps[2];check(i.context.api.GetQueryPoolResults(i.context.device,f.queries,0,2,sizeof(stamps),stamps,8,VK_QUERY_RESULT_64_BIT),"drain final diagnostic GPU timestamps");
        result[n]={f.serial,double(stamps[1]-stamps[0])*i.context.selected.properties.limits.timestampPeriod/1000000.};
    }return result;
}
std::string Backend::deviceReport() const {
    auto& i = *impl;VkMemoryPropertyFlags uploadProperties=0;vmaGetAllocationMemoryProperties(i.context.allocator,i.frames[0].arena.allocation,&uploadProperties);
    auto json = i.context.json(); json.pop_back(); return json + ",\"frames_in_flight\":" + std::to_string(i.config.framesInFlight) + ",\"recording_workers\":" + std::to_string(i.config.dispatchRecording?i.config.recordingWorkers:1) + ",\"upload_memory_flags\":"+std::to_string(uploadProperties)+",\"presentation_mode\":\"" + (i.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : i.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo") + "\",\"pipeline_count\":" + std::to_string(i.pipelines.size()) + "}";
}
unsigned Backend::validationErrors() const { return impl->context.validationErrors.load(std::memory_order_relaxed); }
std::uint64_t Backend::deviceLocalMemory() const {return impl->context.selected.localBytes;}
void Backend::adoptOwnerAtBarrier() {auto& i=*impl;if(i.frame)throw std::runtime_error("Graphics owner handoff inside a frame");if(i.owner!=std::this_thread::get_id()){i.wait(i.lastSubmitted);i.owner=std::this_thread::get_id();}}
void Backend::cancelFrame() noexcept {
    auto& i=*impl;i.recordingFailed=true;
    if(i.frame&&i.owner==std::this_thread::get_id()){
        if(i.frame->serial!=i.nextSerial)i.context.api.ResetCommandPool(i.context.device,i.frame->pool,0);
        i.prepared.clear();i.frame=nullptr;i.activeOcclusion={};i.renderingColor={};i.renderingDepth={};
    }
}
}
