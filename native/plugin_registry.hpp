#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace khz::plugins {
inline constexpr unsigned MaxPlugins = 16, MaxScriptBindings = MaxPlugins * 64;
inline constexpr unsigned MaxCommands = MaxPlugins * 20, MaxExports = MaxPlugins * 50;
inline constexpr unsigned PublicEventCount = 35;
enum class State : std::uint8_t { Empty, Initializing, Enabled, Disabled, UnloadPending };
enum class Kind : std::uint8_t { Function, Method, Command, Export };
enum class RegistrationResult { Ok, Invalid, Duplicate, Full, OwnerInactive };
struct Plugin { char name[64]{}; State state = State::Empty; unsigned callbacks = 0, imports = 0; std::uint64_t generation = 0; };
struct Binding {
    char name[64]{};
    std::uintptr_t callback = 0;
    int owner = -1, power = 100;
    Kind kind = Kind::Function;
    bool replace = false, retired = false;
};
inline bool validName(std::string_view name) {
    if (name.empty() || name.size() >= 64) return false;
    for (unsigned char c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
inline bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i != a.size(); ++i) {
        char ac = a[i], bc = b[i];
        if (ac >= 'A' && ac <= 'Z') ac += 32;
        if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return false;
    }
    return true;
}
class Registry {
public:
    int BeginLoad(std::string_view name) {
        if (!validName(name)) return -1;
        for (const auto& plugin : plugins_) if (plugin.state != State::Empty && sameName(plugin.name, name)) return -1;
        for (unsigned i = 0; i != plugins_.size(); ++i) if (plugins_[i].state == State::Empty) {
            auto& plugin = plugins_[i]; plugin = {}; std::memcpy(plugin.name, name.data(), name.size());
            plugin.state = State::Initializing; plugin.generation = ++generation_; return int(i);
        }
        return -1;
    }
    bool Enable(int owner) { auto* plugin = get(owner); if (!plugin || plugin->state != State::Initializing) return false; plugin->state = State::Enabled; return true; }
    bool Disable(int owner) { auto* plugin = get(owner); if (!plugin || plugin->state == State::Empty || plugin->state == State::UnloadPending) return false; plugin->state = State::Disabled; return true; }
    bool RequestUnload(int owner) {
        auto* plugin = get(owner); if (!plugin || plugin->state == State::Empty) return false;
        plugin->state = State::UnloadPending; return true;
    }
    bool CanUnload(int owner) const {
        const auto* plugin = Get(owner); return plugin && plugin->state == State::UnloadPending && !plugin->callbacks && !plugin->imports;
    }
    bool FinishUnload(int owner) {
        if (!CanUnload(owner)) return false;
        for (auto& binding : scripts_) if (binding.owner == owner) retire(binding);
        for (auto& binding : commands_) if (binding.owner == owner) binding = {};
        for (auto& binding : exports_) if (binding.owner == owner) binding = {};
        plugins_[owner] = {}; return true;
    }
    bool Enter(int owner) {
        auto* plugin = get(owner);
        if (!plugin || (plugin->state != State::Enabled && plugin->state != State::Initializing)) return false;
        ++plugin->callbacks; return true;
    }
    // Public GetVersion queries OnInfoRequest on a loaded disabled module.
    // This narrow metadata call still pins its lifetime; it does not enable
    // ordinary event/script dispatch into that module.
    bool EnterMetadata(int owner) {
        auto* plugin = get(owner); if (!plugin || plugin->state == State::Empty || plugin->state == State::UnloadPending) return false;
        ++plugin->callbacks; return true;
    }
    bool EnterUnload(int owner) {
        auto* plugin = get(owner); if (!plugin || plugin->state != State::UnloadPending || plugin->callbacks) return false;
        ++plugin->callbacks; return true;
    }
    void Leave(int owner) { auto* plugin = get(owner); if (plugin && plugin->callbacks) --plugin->callbacks; }
    bool PinImport(int owner) { auto* plugin = get(owner); if (!plugin || (plugin->state != State::Enabled && plugin->state != State::Initializing)) return false; ++plugin->imports; return true; }
    void ReleaseImport(int owner) { auto* plugin = get(owner); if (plugin && plugin->imports) --plugin->imports; }
    const Plugin* Get(int owner) const { return owner >= 0 && unsigned(owner) < plugins_.size() ? &plugins_[owner] : nullptr; }
    int FindPlugin(std::string_view name) const { for (unsigned i = 0; i != plugins_.size(); ++i) if (plugins_[i].state != State::Empty && sameName(name, plugins_[i].name)) return int(i); return -1; }
    RegistrationResult AddScript(int owner, std::string_view name, Kind kind, std::uintptr_t callback, bool replace, unsigned& slot) {
        if (kind != Kind::Function && kind != Kind::Method) return RegistrationResult::Invalid;
        return add(scripts_, owner, name, kind, callback, replace, 100, 64, slot);
    }
    RegistrationResult AddCommand(int owner, std::string_view name, std::uintptr_t callback, int power, unsigned& slot) {
        return add(commands_, owner, name, Kind::Command, callback, false, power, 20, slot);
    }
    RegistrationResult AddExport(int owner, std::string_view name, std::uintptr_t callback, unsigned& slot) {
        return add(exports_, owner, name, Kind::Export, callback, false, 100, 50, slot);
    }
    const Binding* FindScript(std::string_view name, Kind kind, unsigned* slot = nullptr) const { return find(scripts_, name, kind, slot); }
    const Binding* FindCommand(std::string_view name, unsigned* slot = nullptr) const { return find(commands_, name, Kind::Command, slot); }
    const Binding* FindExport(int owner, std::string_view name, unsigned* slot = nullptr) const {
        for (unsigned i = 0; i != exports_.size(); ++i) if (exports_[i].owner == owner && active(exports_[i]) && sameName(name, exports_[i].name)) {
            if (slot) *slot = i; return &exports_[i];
        }
        return nullptr;
    }
    const Binding* Script(unsigned slot) const { return slot < scripts_.size() && active(scripts_[slot]) ? &scripts_[slot] : nullptr; }
    const Binding* Command(unsigned slot) const { return slot < commands_.size() && active(commands_[slot]) ? &commands_[slot] : nullptr; }
    bool RemoveCommand(int owner, std::string_view name) {
        for (auto& binding : commands_) if (binding.owner == owner && sameName(name, binding.name)) { binding = {}; return true; }
        return false;
    }
    // Invoke only after all compiled VM function pointers have been discarded.
    void VmShutdownBarrier() { for (auto& binding : scripts_) if (binding.retired) binding = {}; }
    unsigned Loaded() const { unsigned count = 0; for (const auto& plugin : plugins_) if (plugin.state != State::Empty) ++count; return count; }
    std::uint64_t LoadGeneration() const { return generation_; }
    unsigned RetiredScripts() const { unsigned count = 0; for (const auto& binding : scripts_) if (binding.retired) ++count; return count; }
    // Engine errors use longjmp. Clear only on the authoritative owner after it
    // has unwound all plugin calls; import/job pins still protect live workers.
    void ErrorCleanup() { for (auto& plugin : plugins_) plugin.callbacks = 0; }
private:
    Plugin* get(int owner) { return owner >= 0 && unsigned(owner) < plugins_.size() ? &plugins_[owner] : nullptr; }
    bool active(const Binding& binding) const {
        const auto* plugin = Get(binding.owner);
        return binding.callback && !binding.retired && plugin && (plugin->state == State::Enabled || plugin->state == State::Initializing);
    }
    static void retire(Binding& binding) { binding.callback = 0; binding.owner = -1; binding.retired = true; }
    template<std::size_t N> RegistrationResult add(std::array<Binding, N>& bindings, int owner, std::string_view name,
        Kind kind, std::uintptr_t callback, bool replace, int power, unsigned ownerLimit, unsigned& index) {
        const auto* plugin = Get(owner);
        if (!plugin || (plugin->state != State::Enabled && plugin->state != State::Initializing)) return RegistrationResult::OwnerInactive;
        if (!validName(name) || !callback || power < 1 || power > 100) return RegistrationResult::Invalid;
        unsigned count = 0; Binding* available = nullptr;
        for (const auto& binding : bindings) if (binding.owner == owner && binding.callback) ++count;
        for (unsigned i = 0; i != bindings.size(); ++i) {
            auto& binding = bindings[i];
            const bool sameScope = kind != Kind::Export || binding.owner == owner;
            if (!binding.retired && binding.callback && sameScope && binding.kind == kind && sameName(binding.name, name)) {
                if (!replace) return RegistrationResult::Duplicate;
                if (binding.owner != owner && count >= ownerLimit) return RegistrationResult::Full;
                binding.callback = callback; binding.owner = owner; binding.replace = true; index = i; return RegistrationResult::Ok;
            }
            if (!binding.callback && !binding.retired && !available) { available = &binding; index = i; }
        }
        if (!available || count >= ownerLimit) return RegistrationResult::Full;
        *available = {}; std::memcpy(available->name, name.data(), name.size()); available->owner = owner;
        available->kind = kind; available->callback = callback; available->power = power; available->replace = replace;
        return RegistrationResult::Ok;
    }
    template<std::size_t N> const Binding* find(const std::array<Binding, N>& bindings, std::string_view name, Kind kind, unsigned* index) const {
        for (unsigned i = 0; i != bindings.size(); ++i) if (bindings[i].kind == kind && active(bindings[i]) && sameName(bindings[i].name, name)) {
            if (index) *index = i; return &bindings[i];
        }
        return nullptr;
    }
    std::array<Plugin, MaxPlugins> plugins_{};
    std::array<Binding, MaxScriptBindings> scripts_{};
    std::array<Binding, MaxCommands> commands_{};
    std::array<Binding, MaxExports> exports_{};
    std::uint64_t generation_ = 0;
};
struct MaintenanceCadence {
    std::uint64_t nextSecond = 0, nextTenSeconds = 0;
    bool initialized = false;
    unsigned Due(std::uint64_t now) {
        if (!initialized) { initialized = true; nextSecond = now + 1000; nextTenSeconds = now + 10000; return 0; }
        unsigned result = 0;
        if (now >= nextSecond) { result |= 1; nextSecond += ((now - nextSecond) / 1000 + 1) * 1000; }
        if (now >= nextTenSeconds) { result |= 2; nextTenSeconds += ((now - nextTenSeconds) / 10000 + 1) * 10000; }
        return result;
    }
};
}
