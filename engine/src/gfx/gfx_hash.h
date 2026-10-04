#pragma once
#include <span>
#include <string>
#include <cstddef>
namespace gfx { std::string sha256(std::span<const std::byte> data); }
