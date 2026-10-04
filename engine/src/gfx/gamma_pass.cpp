#include "gamma_pass.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>

namespace gfx {
namespace {
std::vector<std::uint32_t> compile(const char* source, const char* profile) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    const auto result = D3DCompile(source, std::strlen(source), "cod4x1khz presentation gamma",
        nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    const std::string diagnostic = errors
        ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
    if (errors) errors->Release();
    if (FAILED(result)) { if (code) code->Release(); throw std::runtime_error("Gamma shader: " + diagnostic); }
    std::vector<std::uint32_t> words(code->GetBufferSize() / 4);
    std::memcpy(words.data(), code->GetBufferPointer(), code->GetBufferSize());
    code->Release(); return words;
}
}
GammaPass::GammaPass(Backend& backend) : backend(backend) {
    for (auto& channel : ramp) for (unsigned i = 0; i < 256; ++i) channel[i] = std::uint16_t(i * 257);
}
GammaPass::~GammaPass() {
    // Backend lifetime follows this object. Destruction only occurs at the
    // acknowledged renderer lifetime barrier, with no command buffer open.
    for (auto id : {target, lookup, vertices, indices, vertex, pixel}) if (id) {
        try { backend.release(id); } catch (...) {}
    }
}
void GammaPass::setRamp(const Ramp& value) { if (ramp != value) { ramp = value; dirty = true; } }
bool GammaPass::identity() const {
    for (const auto& channel : ramp) for (unsigned i = 0; i < 256; ++i)
        if (channel[i] != i * 257) return false;
    return true;
}
void GammaPass::prepare(ResourceId source, unsigned w, unsigned h) {
    if (!source || !w || !h) throw std::runtime_error("Invalid gamma presentation source");
    if (!lookup) {
        ImageDesc lut; lut.width = 256; lut.height = 1; lut.format = ImageFormat::Rgba16Unorm;
        lookup = backend.createImage(lut);
        const float geometry[][6] = {{-1,1,0,1,0,0},{1,1,0,1,1,0},{1,-1,0,1,1,1},{-1,-1,0,1,0,1}};
        const std::uint16_t index[] = {0,1,2,0,2,3};
        vertices = backend.createBuffer(std::as_bytes(std::span(geometry)), false);
        indices = backend.createBuffer(std::as_bytes(std::span(index)), true);
        const auto vs = compile("struct I{float4 p:POSITION;float2 t:TEXCOORD0;};"
            "struct O{float4 p:POSITION;float2 t:TEXCOORD0;};"
            "O main(I i){O o;o.p=i.p;o.t=i.t;return o;}", "vs_3_0");
        const auto ps = compile("sampler2D image:register(s0);sampler2D ramp:register(s1);"
            "float4 texel:register(c0);float4 main(float2 t:TEXCOORD0):COLOR0{"
            "float4 c=tex2D(image,t+texel.xy);float3 u=c.rgb*(255.0/256.0)+(0.5/256.0);"
            "return float4(tex2D(ramp,float2(u.r,0.5)).r,"
            "tex2D(ramp,float2(u.g,0.5)).g,tex2D(ramp,float2(u.b,0.5)).b,c.a);}", "ps_3_0");
        vertex = backend.createShader(vs, "presentation gamma vertex");
        pixel = backend.createShader(ps, "presentation gamma pixel");
    }
    if (w != width || h != height) {
        ImageDesc image; image.width = w; image.height = h; image.renderTarget = true; image.sampled = false;
        const auto replacement = backend.createImage(image);
        if (target) backend.release(target);
        target = replacement; width = w; height = h;
    }
    texel = {.5f / w, .5f / h, 0, 0};
    packet = {}; packet.vertexShader = vertex; packet.pixelShader = pixel;
    packet.vertexBuffers[0] = vertices; packet.strides[0] = 24; packet.indexBuffer = indices;
    packet.indexCount = 6; packet.elements = elements; packet.colorTarget = target;
    packet.textures[0] = source; packet.textures[1] = lookup;
    packet.pixelConstants = texel; packet.width = float(w); packet.height = float(h);
    packet.state.depthTest = packet.state.depthWrite = packet.state.cull = false;
    for (auto& sampler : packet.samplers) {
        sampler.u = sampler.v = sampler.w = Address::Clamp;
        sampler.maxLod = 0;
    }
    packet.samplers[0].linear = false;
    backend.warm(packet);
}
ResourceId GammaPass::draw() {
    if (!target) throw std::runtime_error("Unprepared gamma presentation pass");
    if (dirty) {
        std::array<std::array<std::uint16_t, 4>, 256> pixels{};
        for (unsigned i = 0; i < 256; ++i) pixels[i] = {ramp[0][i], ramp[1][i], ramp[2][i], 65535};
        backend.uploadImage(lookup, 0, 0, std::as_bytes(std::span(pixels))); dirty = false;
    }
    backend.draw(packet); return target;
}
}
