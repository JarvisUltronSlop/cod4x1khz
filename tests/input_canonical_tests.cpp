#include <input_canonical.hpp>
#include <array>
#include <cstdio>
#include <cstring>

namespace {
struct Button { int down[2]; unsigned downtime, msec; int active, wasPressed; };
struct Binding { char text[1024]{}; bool consoleKey = false; };
khz::canonical::Digest buttons(const Button (&value)[30]) {
    khz::canonical::Hash hash; khz::input_canonical::Buttons(hash, value); return hash.finish();
}
}
int main() {
    unsigned cases = 0, failures = 0;
    const auto check = [&](bool passed) { ++cases; if (!passed) ++failures; };
    Button baseline[30]{}; const auto original = buttons(baseline);
    check(original.valid && khz::canonical::equal(original, buttons(baseline)));
    // Every reviewed field of every real-sized button participates. This
    // catches omitted held keys, tap latches and sub-frame timing fields.
    for (unsigned index = 0; index < 30; ++index) {
        for (unsigned field = 0; field < 6; ++field) {
            Button changed[30]{};
            switch (field) {
            case 0: changed[index].down[0] = 17; break;
            case 1: changed[index].down[1] = 18; break;
            case 2: changed[index].downtime = 0xFFFFFFFEu; break;
            case 3: changed[index].msec = 1; break;
            case 4: changed[index].active = 1; break;
            case 5: changed[index].wasPressed = 1; break;
            }
            check(!khz::canonical::equal(original, buttons(changed)));
        }
    }
    std::array<Binding, 4> bindings{}; unsigned char keyMap[4][2]{};
    std::strcpy(bindings[1].text, "+attack");
    const auto identity = khz::input_canonical::BindingsIdentity(bindings, keyMap);
    check(identity.valid);
    bindings[1].text[1023] = 'x'; // inactive storage does not affect a name
    check(khz::canonical::equal(identity, khz::input_canonical::BindingsIdentity(bindings, keyMap)));
    bindings[1].consoleKey = true;
    check(!khz::canonical::equal(identity, khz::input_canonical::BindingsIdentity(bindings, keyMap)));
    bindings[1].consoleKey = false; keyMap[3][1] = 9;
    check(!khz::canonical::equal(identity, khz::input_canonical::BindingsIdentity(bindings, keyMap)));
    keyMap[3][1] = 0; std::strcpy(bindings[1].text, "+melee");
    check(!khz::canonical::equal(identity, khz::input_canonical::BindingsIdentity(bindings, keyMap)));
    std::memset(bindings[0].text, 'x', sizeof(bindings[0].text));
    check(!khz::input_canonical::BindingsIdentity(bindings, keyMap).valid);
    std::printf("KHZ_INPUT_CANONICAL_TEST {\"cases\":%u,\"failures\":%u,\"scope\":\"named private held-input serializer and frozen bindings; restoration/game replay require separate evidence\"}\n", cases, failures);
    return failures ? 1 : 0;
}
