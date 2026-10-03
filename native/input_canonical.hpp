#pragma once
#include <canonical.hpp>
#include <cstddef>

namespace khz::input_canonical {
// Read named native fields; no padding, stale binding buffer tails or process
// addresses enter replay identity. Used with the real MP kbutton array.
template<class Button, std::size_t Count>
void Buttons(canonical::Hash& hash, const Button (&buttons)[Count]) {
    hash.u32(static_cast<std::uint32_t>(Count));
    for (const auto& button : buttons) {
        hash.array(button.down);
        hash.scalar(button.downtime); hash.scalar(button.msec);
        hash.scalar(button.active); hash.scalar(button.wasPressed);
    }
}
template<class Bindings, std::size_t Count>
canonical::Digest BindingsIdentity(const Bindings& bindings, const unsigned char (&keyMap)[Count][2]) {
    canonical::Hash hash;
    hash.text("native MP frozen bindings v1");
    hash.u32(static_cast<std::uint32_t>(bindings.size()));
    for (const auto& binding : bindings) {
        hash.text(binding.text, sizeof(binding.text));
        hash.boolean(binding.consoleKey);
    }
    hash.array(keyMap);
    return hash.finish();
}
} // namespace khz::input_canonical
