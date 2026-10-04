#pragma once
#include <cstdint>
#include <algorithm>
#include <limits>

namespace khz {
// Adjacent pre-tess indices may share one draw only when both vertex streams
// and lighting bindings address the same data. Preserve their original order.
struct BspPreTessRun {
    std::uint32_t firstVertex, layerOffset, probe, lightmap;
    std::uint32_t baseIndex, triangles, vertices;
};
inline bool appendBspPreTessRun(BspPreTessRun& run, const BspPreTessRun& next, bool layered) noexcept {
    if (run.firstVertex != next.firstVertex || (layered && run.layerOffset != next.layerOffset) ||
        run.probe != next.probe || run.lightmap != next.lightmap ||
        std::uint64_t(run.baseIndex) + std::uint64_t(run.triangles) * 3 != next.baseIndex ||
        std::uint64_t(run.triangles) + next.triangles > (std::numeric_limits<std::uint32_t>::max)()) return false;
    run.triangles += next.triangles;
    run.vertices = (std::max)(run.vertices, next.vertices);
    return true;
}
}
