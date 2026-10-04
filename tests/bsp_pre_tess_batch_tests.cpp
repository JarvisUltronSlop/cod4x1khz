#include "bsp_pre_tess_batch.hpp"
#include <cassert>
#include <limits>
int main() {
    const khz::BspPreTessRun initial{100, 800, 3, 4, 20, 2, 10};
    const khz::BspPreTessRun adjacent{100, 800, 3, 4, 26, 3, 19};
    auto run = initial;
    assert(khz::appendBspPreTessRun(run, adjacent, true));
    assert(run.baseIndex == 20 && run.triangles == 5 && run.vertices == 19);
    for (int field = 0; field != 5; ++field) {
        auto next = adjacent; auto unchanged = initial;
        switch (field) { case 0: ++next.firstVertex; break; case 1: ++next.layerOffset; break;
            case 2: ++next.probe; break; case 3: ++next.lightmap; break; case 4: ++next.baseIndex; break; }
        assert(!khz::appendBspPreTessRun(unchanged, next, true));
        assert(unchanged.triangles == initial.triangles && unchanged.vertices == initial.vertices);
    }
    auto next = adjacent; ++next.layerOffset; run = initial;
    assert(khz::appendBspPreTessRun(run, next, false));
    run = initial; run.baseIndex = std::numeric_limits<std::uint32_t>::max() - 2;
    next = adjacent; next.baseIndex = 3;
    assert(!khz::appendBspPreTessRun(run, next, false)); // index arithmetic cannot wrap
    run = initial; next = adjacent; next.triangles = std::numeric_limits<std::uint32_t>::max();
    assert(!khz::appendBspPreTessRun(run, next, false));
}
