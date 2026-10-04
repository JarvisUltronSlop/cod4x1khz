#pragma once
#include "gfx_backend.h"

namespace gfx {
// Stock fullscreen gamma is a presentation transform. It must never alter the
// scene surface, screenshot pixels, or the desktop's global gamma ramp.
class GammaPass {
public:
    using Ramp = std::array<std::array<std::uint16_t, 256>, 3>;
    explicit GammaPass(Backend& backend);
    ~GammaPass();
    void setRamp(const Ramp& ramp);
    const Ramp& getRamp() const { return ramp; }
    bool identity() const;
    // Resource creation and pipeline warming happen outside command recording.
    void prepare(ResourceId source, unsigned width, unsigned height);
    // Called after all scene packets, inside the same frame submission.
    ResourceId draw();
private:
    Backend& backend;
    Ramp ramp;
    bool dirty = true;
    ResourceId target, lookup, vertices, indices, vertex, pixel;
    unsigned width = 0, height = 0;
    std::array<VertexElement, 2> elements{{
        {0, 0, 0, VertexFormat::Float4, Semantic::Position},
        {0, 16, 0, VertexFormat::Float2, Semantic::Texcoord}}};
    std::array<float, 4> texel{};
    DrawPacket packet;
};
}
