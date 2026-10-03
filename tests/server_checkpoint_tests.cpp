#include <server_checkpoint.hpp>
#include <array>
#include <cstdio>
#include <cstring>

namespace {
using namespace khz::server_checkpoint;
int checks, failures;
void check(bool condition, const char* label) {
    ++checks; if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}
struct Record { unsigned value, generation; const Record* link; };
Record live[DomainCount][MaxParts], image[DomainCount][MaxParts];
unsigned captures[DomainCount][MaxParts], validations[DomainCount][MaxParts];
unsigned restores[DomainCount][MaxParts], discards[DomainCount][MaxParts];
bool rejectCapture, rejectValidation;
unsigned rejectedDomain, rejectedPart;
unsigned commitDomains[DomainCount * MaxParts], commitCount;
template<unsigned D, unsigned P> bool capture(const Frame&, char* reason, unsigned capacity) {
    ++captures[D][P]; image[D][P] = live[D][P];
    if (rejectCapture && rejectedDomain == D && rejectedPart == P) {
        std::snprintf(reason, capacity, "actual_capture_refused"); return false;
    }
    return true;
}
template<unsigned D, unsigned P> bool validate(const Frame&, char* reason, unsigned capacity) {
    ++validations[D][P];
    if (rejectValidation && rejectedDomain == D && rejectedPart == P) {
        std::snprintf(reason, capacity, "actual_validation_refused"); return false;
    }
    return true;
}
template<unsigned D, unsigned P> void restore(const Frame&) {
    ++restores[D][P]; commitDomains[commitCount++] = D; live[D][P] = image[D][P];
}
template<unsigned D, unsigned P> void discard() { ++discards[D][P]; }
template<unsigned D, unsigned P> Provider provider() {
    return {Domain(D), P, sizeof(Record), capture<D, P>, validate<D, P>, restore<D, P>, discard<D, P>};
}
template<unsigned D = 0> void registerAll(Coordinator& c) {
    check(c.registerProvider(provider<D, 0>()), "mandatory native provider registration");
    if constexpr (RequiredParts[D] == 3) check(c.registerProvider(provider<D, 1>()), "mandatory graph/allocator second part");
    if constexpr (D + 1 < DomainCount) registerAll<D + 1>(c);
}
Frame frame() {
    Frame f;
    f.fixture = 1; f.session = 2; f.mapGeneration = 3; f.fenceGeneration = 4; f.logicalTick = 400;
    f.nativeTime = 400; f.serverId = 7; f.mapChecksum = 19;
    f.serverAddress = 0x100; f.clientsAddress = 0x200; f.entitiesAddress = 0x300;
    f.scriptProgramAddress = 0x400; f.mtBufferAddress = 0x500; f.hunkBaseAddress = 0x600;
    f.hunkLowUsed = 4096; f.hunkHighUsed = 512; return f;
}
void initializeRecords() {
    std::memset(captures, 0, sizeof(captures)); std::memset(validations, 0, sizeof(validations));
    std::memset(restores, 0, sizeof(restores)); std::memset(discards, 0, sizeof(discards));
    commitCount = 0; rejectCapture = rejectValidation = false;
    for (unsigned d = 0; d < DomainCount; ++d) for (unsigned p = 0; p < MaxParts; ++p)
        live[d][p] = {1000 + d * 10 + p, 90 + d, &live[(d + 1) % DomainCount][p]};
}
unsigned totalRestores() {
    unsigned n = 0; for (const auto& d : restores) for (const auto p : d) n += p; return n;
}
void mandatoryAndIdentity() {
    auto f = frame(); Coordinator incomplete;
    check(incomplete.registerProvider(provider<0, 0>()), "one real provider registers");
    check(!incomplete.capture(f) && incomplete.failure() == Failure::MissingProvider && !incomplete.pinned(),
        "partial domain set cannot create restoration claim");
    check(!incomplete.requireParts(Domain::Graphs, 1), "required graph second part cannot be waived");
    check(!incomplete.registerProvider(provider<0, 0>()), "duplicate provider rejected");
    Coordinator c; registerAll(c); check(c.registeredDomains() == AllDomains, "all fourteen domains required");
    auto missing = f; missing.mtBufferAddress = 0;
    check(!c.capture(missing) && !c.pinned(), "missing native allocator identity refuses capture");
    check(c.capture(f), "complete real provider set captures");
    check(!c.registerProvider(provider<0, 1>()), "provider topology sealed while pinned");
    const std::array<std::uintptr_t Frame::*, 6> addresses{&Frame::serverAddress, &Frame::clientsAddress,
        &Frame::entitiesAddress, &Frame::scriptProgramAddress, &Frame::mtBufferAddress, &Frame::hunkBaseAddress};
    for (const auto member : addresses) {
        auto changed = f; ++(changed.*member);
        check(!c.restore(changed) && totalRestores() == 0, "changed original native arena prevents every live write");
    }
    auto changed = f; ++changed.mapGeneration; check(!c.restore(changed), "map generation guarded");
    changed = f; ++changed.fenceGeneration; check(!c.restore(changed), "actual fence generation guarded");
    changed = f; ++changed.session; check(!c.restore(changed), "session guarded");
    changed = f; ++changed.hunkLowUsed; check(!c.restore(changed), "hunk low allocation watermark guarded");
    changed = f; ++changed.hunkHighUsed; check(!c.restore(changed), "hunk high allocation watermark guarded");
    check(c.discard() && !c.pinned(), "rejected restore retains controlled discard");
    Coordinator expanded; registerAll(expanded);
    check(expanded.requireParts(Domain::World, 3), "world aggregator can require additional actual native storage");
    check(!expanded.capture(f) && expanded.failure() == Failure::MissingProvider,
        "expanded world coverage remains mandatory before any capture");
    check(expanded.registerProvider(provider<0, 1>()) && expanded.capture(f), "actual second world provider satisfies strengthened coverage");
    check(expanded.discard(), "expanded world baseline discarded");
}
void atomicValidationAndActualRestore() {
    initializeRecords(); auto f = frame(); Coordinator c; registerAll(c); check(c.capture(f), "capture original pointer-bearing records");
    for (auto& d : live) for (auto& p : d) { p.value += 27; p.generation += 1; p.link = nullptr; }
    rejectedDomain = unsigned(Domain::Dvars); rejectedPart = 0; rejectValidation = true;
    check(!c.restore(f) && totalRestores() == 0 && !c.restoreComplete(), "last-domain refusal causes zero partial commits");
    check(live[0][0].value == 1027 && live[0][0].link == nullptr, "validation leaves actual graph untouched");
    check(c.pinned() && c.capturedDomains() == AllDomains, "failed validation keeps original baseline pinned");
    rejectValidation = false;
    auto later = f; later.logicalTick += 123; later.nativeTime += 123;
    check(c.restore(later) && c.restoreComplete() && c.restoredDomains() == AllDomains,
        "same-process original baseline restores all real records after a branch");
    check(commitCount == DomainCount + 3 && commitDomains[0] == unsigned(Domain::ScriptAllocators) &&
        commitDomains[1] == unsigned(Domain::ScriptAllocators), "native allocator commits precede pointer-bearing graphs");
    for (unsigned d = 0; d < DomainCount; ++d) for (unsigned p = 0; p < MaxParts; ++p)
        if (RequiredParts[d] & (1u << p)) check(live[d][p].value == 1000 + d * 10 + p &&
            live[d][p].generation == 90 + d && live[d][p].link == &live[(d + 1) % DomainCount][p],
            "actual named payload and original-address link restored");
    live[unsigned(Domain::World)][0].value = 77; live[unsigned(Domain::ScriptVM)][1].link = nullptr;
    check(c.restore(later) && c.restorations() == 2 && live[unsigned(Domain::World)][0].value == 1000 &&
        live[unsigned(Domain::ScriptVM)][1].link == image[unsigned(Domain::ScriptVM)][1].link,
        "repeat restore always installs the same original baseline");
    check(c.discard() && !c.restoreComplete() && !c.pinned(), "discard clears restore attestation");
}
void rejectedCaptureAndRanges() {
    initializeRecords(); Coordinator c; registerAll(c); auto f = frame();
    rejectCapture = true; rejectedDomain = unsigned(Domain::ScriptVM); rejectedPart = 1;
    check(!c.capture(f) && !c.pinned() && c.capturedDomains() == 0, "partial capture cleans provider-owned images");
    check(discards[rejectedDomain][rejectedPart] == 1 && discards[0][0] == 1,
        "capture rejection releases both successful and rejecting provider pins");
    rejectCapture = false; check(c.capture(f), "fresh capture after bounded cleanup"); check(c.discard(), "fresh capture discard");
    Ranges r;
    check(r.add(Domain::World, live, sizeof(live)), "register actual restored native graph span");
    check(r.contains(Domain::World, &live[4][3], sizeof(Record)), "covered original-address graph member accepted");
    check(!r.contains(Domain::Animation, &live[4][3], sizeof(Record)), "uncaptured domain reference rejected");
    check(!r.containsAny(reinterpret_cast<const char*>(live) + sizeof(live) - 1, sizeof(Record)), "cross-boundary reference rejected");
    check(!r.containsAny(nullptr, 1) && !r.add(Domain::World, reinterpret_cast<void*>(UINTPTR_MAX - 3), 16),
        "null and address wrap do not pass graph coverage");
    for (unsigned i = 1; i < Ranges::Capacity; ++i) check(r.add(Domain::Runtime, reinterpret_cast<void*>(0x10000 + i * 16), 8), "fixed range capacity registration");
    check(!r.add(Domain::Runtime, reinterpret_cast<void*>(0x80000), 8) &&
        r.add(Domain::World, live, sizeof(live)), "range overflow rejected and exact existing registration remains idempotent");
    Branch branch; check(branch.eligible(), "fresh branch eligible");
    branch.reject("native_bot_file_write"); branch.reject("native_string_mutation");
    check(!branch.eligible() && branch.rejections() == 2 && !std::strcmp(branch.reason(), "native_bot_file_write"),
        "first unsupported effect latches diagnostic without losing original image");
    check(c.capture(f) && c.restore(f) && !branch.eligible(), "baseline cleanup restore cannot clear invalid replay branch");
    check(c.discard() && !branch.eligible(), "discard is not replay attestation");
    branch.reset(); check(branch.eligible() && branch.rejections() == 2 && !branch.reason()[0], "explicit fresh fixture resets only branch eligibility");
}
void exactMapLifetimeRanges() {
    Ranges ranges;
    check(ranges.add(Domain::Animation, live, sizeof(live)), "static animation arena enrolled");
    const void* oldTrees[] = {reinterpret_cast<void*>(0x10000), reinterpret_cast<void*>(0x10040)};
    const std::size_t sizes[] = {20, 20};
    check(ranges.replaceMap(Domain::Animation, oldTrees, sizes, 2), "exact original native tree spans enrolled");
    const void* collision[] = {reinterpret_cast<void*>(0x20000)};
    check(ranges.replaceMap(Domain::Collision, collision, sizes, 1), "separate map provider enrolled");
    const void* newTrees[] = {reinterpret_cast<void*>(0x30000), reinterpret_cast<void*>(0x30040)};
    check(ranges.replaceMap(Domain::Animation, newTrees, sizes, 2), "next map replaces animation tree spans atomically");
    check(!ranges.containsAny(oldTrees[0], 1) && !ranges.containsAny(oldTrees[1], 1), "old map tree addresses cease to be restorable");
    check(ranges.contains(Domain::Animation, newTrees[0], 20) &&
        !ranges.contains(Domain::Animation, reinterpret_cast<void*>(0x30013), 2), "exact new tree allocation excludes alignment padding");
    check(ranges.contains(Domain::Animation, live, sizeof(live)) && ranges.containsAny(collision[0], 20),
        "replacement retains static arena and independent map provider");
    const void* invalid[] = {newTrees[0], reinterpret_cast<void*>(UINTPTR_MAX - 3)};
    check(!ranges.replaceMap(Domain::Animation, invalid, sizes, 2) && ranges.containsAny(newTrees[1], 20),
        "address-wrap replacement refuses before retiring valid bindings");
    const void* aliased[] = {newTrees[0], newTrees[0]};
    check(!ranges.replaceMap(Domain::Animation, aliased, sizes, 2) && ranges.containsAny(newTrees[1], 20),
        "aliased map allocations refuse without a partial registration");
    check(!ranges.replaceMap(Domain::Animation, nullptr, sizes, 2) &&
        !ranges.replaceMap(Domain::Count, newTrees, sizes, 2), "missing map arrays and invalid domain refuse");
    const void* many[Ranges::Capacity]; std::size_t manySizes[Ranges::Capacity];
    for (unsigned i = 0; i < Ranges::Capacity; ++i) { many[i] = reinterpret_cast<void*>(0x40000 + i * 32); manySizes[i] = 20; }
    check(!ranges.replaceMap(Domain::Animation, many, manySizes, Ranges::Capacity) && ranges.containsAny(newTrees[1], 20),
        "capacity refusal preserves all previous map bindings");
    check(ranges.replaceMap(Domain::Animation, nullptr, nullptr, 0) && !ranges.containsAny(newTrees[0], 1) &&
        ranges.containsAny(collision[0], 20), "one-domain retirement retains another provider");
    ranges.resetMap();
    check(!ranges.containsAny(collision[0], 1) && ranges.contains(Domain::Animation, live, sizeof(live)),
        "map reset retires dynamic spans while preserving static native arenas");
    for (unsigned i = 0; i < 1000; ++i)
        check(ranges.replaceMap(Domain::Animation, (i & 1) ? oldTrees : newTrees, sizes, 2),
            "repeated maps reuse bounded range capacity");
}
}
int main() {
    initializeRecords(); mandatoryAndIdentity(); atomicValidationAndActualRestore(); rejectedCaptureAndRanges(); exactMapLifetimeRanges();
    std::printf("{\"checks\":%d,\"failures\":%d,\"required_domains\":%u,\"full_world_replay_verified\":false}\n", checks, failures, AllDomains);
    return failures ? 1 : 0;
}
