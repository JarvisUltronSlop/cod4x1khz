#include "plugin_registry.hpp"
#include "plugin_journal.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
using namespace khz::plugins;
int main() {
    FileJournal<2, 64> journal; FileJournal<2, 64>::Record entry, oldEntry;
    CHECK(!journal.Push("unopened")); CHECK(journal.Bind(7) == 0);
    char stamped[] = "123456 source text";
    CHECK(journal.Push(stamped)); stamped[0] = '9';
    CHECK(journal.Pop(oldEntry)); CHECK(!std::strcmp(oldEntry.text, "123456 source text"));
    CHECK(journal.Active(oldEntry)); CHECK(journal.Push("pending old map"));
    CHECK(journal.Bind(7) == 1); CHECK(!journal.Active(oldEntry)); // native handle reuse
    CHECK(!journal.Pop(entry)); CHECK(journal.Push("new map1")); CHECK(journal.Push("new map2"));
    CHECK(!journal.Push("full")); CHECK(journal.Pop(entry)); CHECK(journal.Active(entry)); CHECK(!std::strcmp(entry.text, "new map1"));
    CHECK(journal.Invalidate() == 1); CHECK(!journal.Active(entry)); CHECK(!journal.Push("after error"));
    CHECK(journal.Bind(8) == 0); CHECK(journal.Push("after restart")); CHECK(journal.Pop(entry)); CHECK(journal.Active(entry));
    auto registry = std::make_unique<Registry>();
    const int first = registry->BeginLoad("regression"); CHECK(first == 0); CHECK(registry->BeginLoad("REGRESSION") < 0);
    CHECK(registry->BeginLoad("../outside") < 0); unsigned index = 0;
    CHECK(registry->AddScript(first, "fixturefunc", Kind::Function, 123, false, index) == RegistrationResult::Ok);
    const auto cachedSlot = index; CHECK(registry->Script(cachedSlot)->callback == 123);
    CHECK(registry->AddScript(first, "fixturefunc", Kind::Function, 124, false, index) == RegistrationResult::Duplicate);
    CHECK(registry->AddScript(first, "fixturefunc", Kind::Function, 125, true, index) == RegistrationResult::Ok);
    CHECK(registry->Script(cachedSlot)->callback == 125);
    CHECK(registry->AddScript(first, "fixturefunc", Kind::Method, 126, false, index) == RegistrationResult::Ok);
    CHECK(registry->AddCommand(first, "fixturecmd", 321, 10, index) == RegistrationResult::Ok);
    CHECK(registry->FindCommand("FIXTURECMD")->power == 10); CHECK(registry->Enable(first));
    CHECK(registry->AddExport(first, "fixtureexport", 456, index) == RegistrationResult::Ok); CHECK(registry->FindExport(first, "fixtureexport"));
    CHECK(registry->PinImport(first)); CHECK(registry->Enter(first)); CHECK(registry->RequestUnload(first)); CHECK(!registry->CanUnload(first));
    CHECK(!registry->EnterMetadata(first)); CHECK(!registry->Disable(first)); // unloading cannot be resurrected by disable
    CHECK(!registry->EnterUnload(first));
    CHECK(!registry->Script(cachedSlot)); CHECK(!registry->Enter(first)); registry->Leave(first); CHECK(!registry->CanUnload(first));
    registry->ReleaseImport(first); CHECK(registry->CanUnload(first));
    CHECK(registry->EnterUnload(first)); CHECK(!registry->CanUnload(first)); CHECK(!registry->EnterUnload(first)); registry->Leave(first);
    CHECK(registry->CanUnload(first)); CHECK(registry->FinishUnload(first));
    CHECK(registry->Loaded() == 0 && registry->RetiredScripts() == 2); CHECK(!registry->Script(cachedSlot));
    const int reused = registry->BeginLoad("newmodule"); CHECK(reused == first);
    CHECK(registry->AddScript(reused, "newfunc", Kind::Function, 999, false, index) == RegistrationResult::Ok); CHECK(index != cachedSlot);
    registry->VmShutdownBarrier(); CHECK(registry->RetiredScripts() == 0); // cached pointer reuse requires this explicit barrier
    CHECK(registry->AddScript(reused, "afterbarrier", Kind::Function, 998, false, index) == RegistrationResult::Ok); CHECK(index == cachedSlot);
    CHECK(registry->Enable(reused)); CHECK(registry->Disable(reused)); CHECK(!registry->FindScript("newfunc", Kind::Function));
    CHECK(!registry->Enter(reused)); CHECK(registry->EnterMetadata(reused)); CHECK(registry->Get(reused)->callbacks == 1); registry->Leave(reused);
    CHECK(registry->AddCommand(reused, "disabled", 1, 100, index) == RegistrationResult::OwnerInactive);
    const int replacer = registry->BeginLoad("replacement_owner"); CHECK(replacer == 1);
    CHECK(registry->AddScript(replacer, "newfunc", Kind::Function, 500, true, index) == RegistrationResult::Ok);
    CHECK(registry->Script(index)->owner == replacer && registry->Script(index)->callback == 500);
    registry = std::make_unique<Registry>();
    for (unsigned plugin = 0; plugin != MaxPlugins; ++plugin) {
        const int owner = registry->BeginLoad("plugin" + std::to_string(plugin)); CHECK(owner == int(plugin));
        for (unsigned function = 0; function != 64; ++function)
            CHECK(registry->AddScript(owner, "fn" + std::to_string(plugin * 64 + function), Kind::Function, function + 1, false, index) == RegistrationResult::Ok);
        CHECK(registry->AddScript(owner, "extra" + std::to_string(plugin), Kind::Function, 100, false, index) == RegistrationResult::Full);
        for (unsigned command = 0; command != 20; ++command)
            CHECK(registry->AddCommand(owner, "cmd" + std::to_string(plugin * 20 + command), 1, 100, index) == RegistrationResult::Ok);
        CHECK(registry->AddCommand(owner, "extra", 1, 100, index) == RegistrationResult::Full);
        CHECK(registry->Enable(owner));
    }
    CHECK(registry->BeginLoad("seventeenth") < 0);
    CHECK(registry->RequestUnload(0)); CHECK(registry->FinishUnload(0)); CHECK(registry->RetiredScripts() == 64);
    CHECK(registry->BeginLoad("replacement") == 0);
    CHECK(registry->AddScript(0, "cannotreusecachedslots", Kind::Function, 1, false, index) == RegistrationResult::Full);
    registry->VmShutdownBarrier(); CHECK(registry->AddScript(0, "reclaimed", Kind::Function, 1, false, index) == RegistrationResult::Ok);
    MaintenanceCadence cadence; CHECK(cadence.Due(17) == 0); CHECK(cadence.Due(1016) == 0); CHECK(cadence.Due(1017) == 1);
    CHECK(cadence.Due(2017) == 1); CHECK(cadence.Due(10017) == 3); CHECK(cadence.Due(10017) == 0);
    CHECK(cadence.Due(45017) == 3); CHECK(cadence.nextSecond == 46017 && cadence.nextTenSeconds == 50017);
    std::puts("plugin registry tests passed");
}
