#include <content_server_policy.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace khz::content::server;
#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); std::exit(1); } } while (0)
Admission admitted(unsigned slot = 0) {
    Admission value; value.slot = slot; value.connected = true; value.lifecycle = 12; value.session = 45; value.mapGeneration = 78; value.fingerprint[0] = 9; return value;
}
int main() {
    const auto original = admitted(); CHECK(Accepted(original)); CHECK(SameConnection(original, original));
    auto changed = original; changed.lifecycle++; CHECK(!SameConnection(original, changed));
    changed = original; changed.session++; CHECK(!SameConnection(original, changed));
    changed = original; changed.mapGeneration++; CHECK(!SameConnection(original, changed));
    changed = original; changed.fingerprint[1]++; CHECK(!SameConnection(original, changed));
    changed = original; changed.connected = false; CHECK(!SameConnection(original, changed));
    changed = original; changed.bot = true; CHECK(!Accepted(changed));
    CHECK(AllowedScope("mods/example/mod.ff", "mods/example", "mp_test"));
    CHECK(AllowedScope("mods/example/assets.iwd", "mods/example", "mp_test"));
    CHECK(!AllowedScope("mods/example2/mod.ff", "mods/example", "mp_test"));
    CHECK(!AllowedScope("mods/other/mod.ff", "mods/example", "mp_test"));
    CHECK(!AllowedScope("main/iw_00.iwd", "mods/example", "mp_test"));
    CHECK(!AllowedScope("main/IW_00.iwd", "", "mp_test")); CHECK(!AllowedScope("main/iW_00.iwd", "", "mp_test"));
    CHECK(AllowedScope("main/khz_fixture.iwd", "", "mp_test"));
    CHECK(!AllowedScope("main/folder/khz_fixture.iwd", "", "mp_test")); CHECK(!AllowedScope("main/mod.ff", "", "mp_test"));
    CHECK(AllowedScope("usermaps/mp_test/mp_test.ff", "", "mp_test"));
    CHECK(AllowedScope("usermaps/mp_test/mp_test_load.ff", "", "mp_test"));
    CHECK(AllowedScope("usermaps/mp_test/mp_test.iwd", "", "mp_test"));
    CHECK(!AllowedScope("usermaps/mp_test/mod.ff", "", "mp_test"));
    CHECK(!AllowedScope("usermaps/mp_other/mp_other.ff", "", "mp_test"));
    Requests requests; Pending pending; pending.admission = original; pending.id = 1; std::strcpy(pending.relative, "mods/example/mod.ff");
    CHECK(requests.Put(pending, 0) == Enqueue::Accepted);
    CHECK(requests.Put(pending, 0) == Enqueue::Duplicate);
    pending.id = 2; CHECK(requests.Put(pending, 100) == Enqueue::Busy);
    Pending taken; CHECK(requests.Take(&taken)); CHECK(taken.id == 1); CHECK(!requests.Take(&taken));
    auto stale = taken; stale.admission.lifecycle--; CHECK(!requests.Finish(stale)); CHECK(requests.Finish(taken));
    CHECK(requests.Put(pending, 50) == Enqueue::Limited); CHECK(requests.Put(pending, 100) == Enqueue::Accepted);
    CHECK(requests.Take(&taken));
    // Reuse of this slot, even by the same identity, admits a new independent
    // request and prevents an old worker completion clearing its in-flight bit.
    pending.admission.lifecycle++; pending.id = 1; CHECK(requests.Put(pending, 101) == Enqueue::Accepted);
    CHECK(!requests.Finish(taken)); CHECK(requests.Take(&taken)); CHECK(requests.Finish(taken));
    pending.id = 2; CHECK(requests.Put(pending, 201) == Enqueue::Accepted); CHECK(requests.Take(&taken));
    stale = taken; requests.Reset(); pending.admission.session++; pending.admission.mapGeneration++; pending.id = 2;
    CHECK(requests.Put(pending, 202) == Enqueue::Accepted); CHECK(requests.Take(&taken));
    CHECK(!requests.Finish(stale)); CHECK(requests.Finish(taken));
    requests.Reset();
    for (unsigned i = 0; i < Slots; ++i) { pending.admission = admitted(i); pending.id = 1; CHECK(requests.Put(pending, 0) == Enqueue::Accepted); }
    for (unsigned i = 0; i < Slots; ++i) { CHECK(requests.Take(&taken)); CHECK(taken.admission.slot == i); }
    CHECK(!requests.Take(&taken));
    requests.Reset(); CHECK(!requests.Take(&taken));
    std::puts("content server policy checks passed");
}
