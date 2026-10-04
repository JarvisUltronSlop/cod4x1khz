#pragma once
#include "vk_shader.h"
#include <filesystem>
namespace gfx::vk {
LinkedShaders cachedLink(const std::filesystem::path& root, const Context& context,
    const Shader& vertex, const Shader& pixel, std::span<const MOJOSHADER_vertexAttribute> attributes);
}
