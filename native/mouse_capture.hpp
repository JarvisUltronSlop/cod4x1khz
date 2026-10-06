#pragma once

namespace khz::mouse {
struct Rect { long left{}, top{}, right{}, bottom{}; };
inline bool Same(const Rect& a, const Rect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
// Main-thread cursor ownership; Raw Input continues to supply relative motion.
// Client bounds are recomputed only on activation, window replacement or move.
template<class Api> class Capture {
    typename Api::Window window{};
    Rect bounds{};
    bool held = false, dirty = true;
public:
    void Invalidate() { dirty = true; }
    void Release(Api& api) {
        if (held) {
            Rect current;
            if (api.GetClip(current) && Same(current, bounds)) api.Clip(nullptr);
            if (api.Captured() == window) api.Release();
        }
        held = false; window = {}; dirty = true;
    }
    bool Update(Api& api, typename Api::Window next, bool gameplay) {
        if (!gameplay || !next || api.Foreground() != next || api.Minimized(next)) {
            Release(api); return false;
        }
        if (window != next) { Release(api); window = next; }
        if (!held || dirty) {
            Rect client;
            if (!api.ClientBounds(next, client) || client.right <= client.left || client.bottom <= client.top || !api.Clip(&client)) {
                Release(api); return false;
            }
            bounds = client; held = true; dirty = false;
        }
        if (api.Captured() != next) api.Acquire(next);
        return true;
    }
};
} // namespace khz::mouse
