#pragma once
#include "vk_context.h"
#include <mojoshader.h>
#include <memory>
#include <vector>
#include <span>
#include <array>

namespace gfx::vk {
struct ParsedShaderDeleter { void operator()(const MOJOSHADER_parseData* p) const { if (p) MOJOSHADER_freeParseData(p); } };
struct Shader {
    std::unique_ptr<const MOJOSHADER_parseData, ParsedShaderDeleter> parsed;
    std::vector<std::uint32_t> bytecode;
    std::string name;
    std::size_t uniformBytes = 0;
    Shader(std::span<const std::uint32_t> source, std::string debugName);
    void packUniforms(void* destination, std::span<const float> floats,
        std::span<const int> integers, std::span<const int> booleans, bool zeroTail=false) const;
    unsigned inputRegister(MOJOSHADER_usage usage, unsigned index) const;
private:
    struct UniformCopy {MOJOSHADER_uniformType type;unsigned index,count,offset;};
    struct UniformLiteral {unsigned offset,bytes;std::array<std::byte,16> value;};
    std::vector<UniformCopy> uniformCopies;
    std::vector<UniformLiteral> uniformLiterals;
};
struct LinkedShaders {
    std::vector<std::uint32_t> vertex, pixel;
};
LinkedShaders link(const Shader& vertex, const Shader& pixel, std::span<const MOJOSHADER_vertexAttribute> attributes);
void validateSpirv(std::span<const std::uint32_t> words, const char* name);
std::vector<std::uint32_t> alphaTest(std::span<const std::uint32_t> pixel, unsigned comparison);
std::vector<std::uint32_t> fragmentState(std::span<const std::uint32_t> pixel,
    int alphaComparison, bool depthBias, bool slopeBias);
std::vector<std::uint32_t> depthCompare(std::span<const std::uint32_t> source,unsigned samplerMask);
}
