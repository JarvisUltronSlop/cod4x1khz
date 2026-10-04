#include <universal/q_shared.h>
#include <gfx_d3d/r_model_skin.h>
#include <xanim/xanim.h>
#include <array>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <xmmintrin.h>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
namespace {
std::uint64_t fingerprint = 14695981039346656037ull;
void append(const void* data, std::size_t bytes) {
    const auto* value = static_cast<const std::uint8_t*>(data);
    while (bytes--) { fingerprint ^= *value++; fingerprint *= 1099511628211ull; }
}
}
int main() {
    alignas(16) std::array<DObjSkelMat, 4> bones{};
    for (unsigned i = 0; i < bones.size(); ++i) {
        bones[i].axis[0][0] = i & 1 ? 0 : 1;
        bones[i].axis[0][1] = i & 1 ? 1 : 0;
        bones[i].axis[1][0] = i & 1 ? -1 : 0;
        bones[i].axis[1][1] = i & 1 ? 0 : 1;
        bones[i].axis[2][2] = 1;
        bones[i].origin[0] = i * 0.25f; bones[i].origin[1] = -int(i) * 0.5f;
        bones[i].origin[2] = i * 0.75f; bones[i].origin[3] = 1;
    }
    alignas(16) std::array<GfxPackedVertex, 64> source{}, output{};
    std::array<GfxPackedVertexNormal, 64> normals{}, supplied{};
    for (unsigned i = 0; i < source.size(); ++i) {
        auto* words = reinterpret_cast<std::uint32_t*>(&source[i]);
        const float position[]{int(i) * 0.125f - 2, int(i % 11) * 0.25f - 1,
            int(i % 7) * 0.5f, i & 1 ? -1.0f : 1.0f};
        std::memcpy(words, position, sizeof(position));
        words[4] = 0x8fa01000u + i; words[5] = 0x30004000u + i;
        words[6] = 0x3f000000u | ((97u + i) << 16) | ((110u + i % 33) << 8) | (80u + i);
        words[7] = 0x3f000000u | ((120u + i) << 16) | ((75u + i % 39) << 8) | (150u - i);
        const std::uint32_t normalWords[]{words[6] ^ 0x00112233u, words[7] ^ 0x00332211u};
        std::memcpy(&supplied[i], normalWords, sizeof(normalWords));
    }
    XSurface surface{}; surface.verts0 = source.data(); surface.vertCount = source.size(); surface.deformed = true;
    std::vector<std::uint16_t> blend;
    for (unsigned extra = 0; extra < 4; ++extra) {
        surface.vertInfo.vertCount[extra] = 16;
        for (unsigned i = 0; i < 16; ++i) {
            blend.push_back((i % 4) * sizeof(DObjSkelMat));
            constexpr std::uint16_t weights[]{0,1,32767,65535};
            for (unsigned w = 1; w <= extra; ++w) {
                blend.push_back(((i + w) % 4) * sizeof(DObjSkelMat)); blend.push_back(weights[(i + w) % 4]);
            }
        }
    }
    surface.vertInfo.vertsBlend = blend.data();
    for (unsigned rounding : {static_cast<unsigned>(_MM_ROUND_NEAREST), static_cast<unsigned>(_MM_ROUND_DOWN),
            static_cast<unsigned>(_MM_ROUND_UP), static_cast<unsigned>(_MM_ROUND_TOWARD_ZERO)}) {
        _MM_SET_ROUNDING_MODE(rounding);
        R_SkinXSurfaceSkinnedSse(&surface, bones.data(), nullptr, normals.data(), output.data());
        append(output.data(), sizeof(output)); append(normals.data(), sizeof(normals));
        for (unsigned i = 0; i < output.size(); ++i)
            CHECK(std::memcmp(reinterpret_cast<const std::byte*>(&output[i]) + 24, &normals[i], sizeof(normals[i])) == 0);
        R_SkinXSurfaceSkinnedSse(&surface, bones.data(), supplied.data(), normals.data(), output.data());
        append(output.data(), sizeof(output)); append(normals.data(), sizeof(normals));
        CHECK(std::memcmp(normals.data(), supplied.data(), sizeof(normals)) == 0);
        surface.deformed = false;
        std::array<XRigidVertList, 4> lists{};
        for (unsigned i = 0; i < lists.size(); ++i) { lists[i].boneOffset = i * sizeof(DObjSkelMat); lists[i].vertCount = 16; }
        surface.vertListCount = lists.size(); surface.vertList = lists.data();
        R_SkinXSurfaceSkinnedSse(&surface, bones.data(), nullptr, normals.data(), output.data());
        append(output.data(), sizeof(output)); append(normals.data(), sizeof(normals));
        R_SkinXSurfaceSkinnedSse(&surface, bones.data(), supplied.data(), normals.data(), output.data());
        append(output.data(), sizeof(output)); append(normals.data(), sizeof(normals));
        CHECK(std::memcmp(normals.data(), supplied.data(), sizeof(normals)) == 0);
        surface.deformed = true;
    }
    _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
    CHECK(fingerprint == 0x46c1572ade84e639ull);
    std::printf("Native model skinning: %zu-bit, all weight counts, rigid/in/out paths, four rounding modes, fingerprint %016llx\n", sizeof(void*) * 8, fingerprint);
}
