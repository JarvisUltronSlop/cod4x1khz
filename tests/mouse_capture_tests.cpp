#include "mouse_capture.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
struct Api {
    using Window = unsigned;
    Window foreground = 1, captured = 0;
    khz::mouse::Rect client{-1800, 80, -520, 800}, clip{-1920, 0, 1920, 1080};
    bool iconic = false, clipFails = false;
    unsigned clips{}, releases{}, boundsReads{};
    Window Foreground() { return foreground; }
    bool Minimized(Window) { return iconic; }
    bool ClientBounds(Window, khz::mouse::Rect& out) { ++boundsReads; out = client; return true; }
    bool GetClip(khz::mouse::Rect& out) { out = clip; return true; }
    bool Clip(const khz::mouse::Rect* rect) { if (clipFails) return false; ++clips; clip = rect ? *rect : khz::mouse::Rect{-1920, 0, 1920, 1080}; return true; }
    Window Captured() { return captured; }
    void Acquire(Window window) { captured = window; }
    void Release() { ++releases; captured = 0; }
};
int main() {
    Api api; khz::mouse::Capture<Api> capture;
    CHECK(capture.Update(api, 1, true) && khz::mouse::Same(api.clip, api.client) && api.captured == 1);
    for (int i = 0; i < 10000; ++i) CHECK(capture.Update(api, 1, true));
    CHECK(api.boundsReads == 1 && api.clips == 1);
    api.captured = 0; CHECK(capture.Update(api, 1, true) && api.captured == 1);
    api.client = {2000, 200, 3280, 920}; capture.Invalidate();
    CHECK(capture.Update(api, 1, true) && khz::mouse::Same(api.clip, api.client));
    CHECK(!capture.Update(api, 1, false) && api.captured == 0 && api.releases == 1);
    CHECK(capture.Update(api, 1, true)); api.foreground = 2;
    CHECK(!capture.Update(api, 1, true) && api.captured == 0);
    api.foreground = 1; CHECK(capture.Update(api, 1, true)); api.iconic = true;
    CHECK(!capture.Update(api, 1, true)); api.iconic = false;
    CHECK(capture.Update(api, 1, true)); api.foreground = 2;
    api.clip = {50, 50, 450, 450}; api.captured = 2;
    CHECK(!capture.Update(api, 1, true) && api.clip.left == 50 && api.captured == 2);
    CHECK(capture.Update(api, 2, true) && api.captured == 2);
    capture.Release(api); CHECK(api.captured == 0);
    api.clipFails = true; CHECK(!capture.Update(api, 2, true));
    api.clipFails = false; api.client = {}; CHECK(!capture.Update(api, 2, true));
    std::puts("Mouse capture covers gameplay, menus, focus loss, movement, rebinding and foreign ownership");
}
