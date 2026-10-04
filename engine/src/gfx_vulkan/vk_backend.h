#pragma once
#include "gfx/gfx_backend.h"
#include "vk_shader.h"

namespace gfx::vk {
class Backend final : public gfx::Backend {
public:
    explicit Backend(const gfx::Config& config);
    ~Backend() override;
    Api api() const noexcept override { return Api::Vulkan; }
    ResourceId createBuffer(std::span<const std::byte>, bool index, bool dynamic = false) override;
    void updateBuffer(ResourceId,unsigned offset,std::span<const std::byte>,bool discard=false,bool noOverwrite=false) override;
    ResourceId createImage(const ImageDesc&) override;
    void uploadImage(ResourceId, unsigned level, unsigned layer, std::span<const std::byte>) override;
    ResourceId createShader(std::span<const std::uint32_t>, std::string) override;
    ResourceId createQuery(QueryType) override;
    void beginQuery(ResourceId) override;
    void endQuery(ResourceId) override;
    bool queryResult(ResourceId,std::uint64_t&) override;
    // Read-only host polling under the facade's lifetime/submission mutex.
    // This does not transfer submission ownership or wait for unrelated work.
    bool pollQuerySerialized(ResourceId,std::uint64_t&);
    void release(ResourceId) override;
    void beginFrame() override;
    void clear(ResourceId color, ResourceId depth, const std::array<float, 4>& rgba, float depthValue,
        unsigned stencilValue=0,bool clearDepth=true,bool clearStencil=true,std::span<const ClearRect> rectangles={}) override;
    void draw(const DrawPacket&) override;
    void warm(const DrawPacket&) override;
    void resolve(ResourceId source,ResourceId destination) override;
    void blit(ResourceId source,ResourceId destination,bool linear=false,
        const ClearRect* sourceRect=nullptr,const ClearRect* destinationRect=nullptr) override;
    FrameTiming endFrame(ResourceId presentationSource) override;
    std::vector<std::byte> readback(ResourceId) override;
    void waitIdle() override;
    std::string deviceReport() const override;
    unsigned validationErrors() const;
    std::uint64_t deviceLocalMemory() const;
    // The engine's acknowledged loading/restart barrier calls this while no
    // frame is open. Existing GPU work completes before the owner changes.
    void adoptOwnerAtBarrier();
    // Fail closed after recording fails: cleanup waits only for submitted work.
    void cancelFrame() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
