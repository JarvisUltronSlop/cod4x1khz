#pragma once
#include "gfx/gfx_backend.h"

namespace gfx::d3d9 {
// Adapter for matched immutable-packet regression tests and the graphics
// boundary migration. Existing stock rendering remains the deployed fallback.
class Backend final : public gfx::Backend {
public:
    explicit Backend(const Config&);
    ~Backend() override;
    Api api() const noexcept override { return Api::D3D9; }
    ResourceId createBuffer(std::span<const std::byte>, bool index, bool dynamic = false) override;
    void updateBuffer(ResourceId,unsigned offset,std::span<const std::byte>,bool discard=false,bool noOverwrite=false) override;
    ResourceId createImage(const ImageDesc&) override;
    void uploadImage(ResourceId, unsigned level, unsigned layer, std::span<const std::byte>) override;
    ResourceId createShader(std::span<const std::uint32_t>, std::string) override;
    ResourceId createQuery(QueryType) override;
    void beginQuery(ResourceId) override;
    void endQuery(ResourceId) override;
    bool queryResult(ResourceId,std::uint64_t&) override;
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
private:
    struct Impl; std::unique_ptr<Impl> impl;
};
}
