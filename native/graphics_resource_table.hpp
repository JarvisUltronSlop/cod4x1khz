#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace gfx {
struct ResourceId {
    std::uint32_t value = 0;
    explicit operator bool() const { return value != 0; }
    friend bool operator==(ResourceId, ResourceId) = default;
};
static_assert(sizeof(ResourceId) == 4);
// GPU objects stay outside the fastfile ABI. Generation exhaustion permanently
// retires a slot rather than making an ancient asset reference valid again.
template<class T, std::size_t Capacity> class ResourceTable {
    static_assert(Capacity > 0 && Capacity < 65535);
    struct Slot { std::unique_ptr<T> object; std::uint16_t generation = 1; bool retired = false; };
    std::array<Slot, Capacity> slots;
    std::size_t cursor = 0, liveCount = 0;
public:
    template<class... Args> ResourceId create(Args&&... args) {
        for (std::size_t n = 0; n < Capacity; ++n) {
            const auto index = (cursor + n) % Capacity; auto& slot = slots[index];
            if (slot.object || slot.retired) continue;
            auto object = std::make_unique<T>(std::forward<Args>(args)...);
            slot.object = std::move(object); ++liveCount; cursor = (index + 1) % Capacity;
            return {std::uint32_t(slot.generation) << 16 | std::uint32_t(index + 1)};
        }
        throw std::runtime_error("Bounded graphics resource table exhausted");
    }
    T* find(ResourceId id) noexcept {
        const auto index = (id.value & 65535u);
        if (!index || index > Capacity) return nullptr;
        auto& slot = slots[index - 1];
        return slot.generation == (id.value >> 16) ? slot.object.get() : nullptr;
    }
    const T* find(ResourceId id) const noexcept { return const_cast<ResourceTable*>(this)->find(id); }
    T& get(ResourceId id) {
        auto* object = find(id); if (!object) throw std::runtime_error("Stale or invalid graphics resource reference");
        return *object;
    }
    std::unique_ptr<T> extract(ResourceId id) noexcept {
        if (!find(id)) return {};
        auto& slot = slots[(id.value & 65535u) - 1]; auto result = std::move(slot.object); --liveCount;
        if (slot.generation == 65535) slot.retired = true; else ++slot.generation;
        return result;
    }
    bool erase(ResourceId id) noexcept { return bool(extract(id)); }
    template<class F> void forEach(F&& visit) { for (auto& slot : slots) if (slot.object) visit(*slot.object); }
    std::size_t size() const noexcept { return liveCount; }
};
}
