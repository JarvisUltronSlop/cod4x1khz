#include <host_checkpoint_policy.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

using namespace khz::host_checkpoint;
int main() {
    Channel native{}; native.active = true; native.unsentBufferSize = 131072;
    assert(channelFailure(native) == ChannelFailure::None);
    // The actual native send completion retains its extent, clears pending,
    // and later small sends reset the cursor without clearing that extent.
    native.unsentLength = 25000; native.unsentFragments = true;
    assert(channelFailure(native) == ChannelFailure::OutgoingFragment);
    native.unsentFragments = false;
    assert(channelFailure(native) == ChannelFailure::None);
    native.unsentLength = native.unsentBufferSize;
    assert(channelFailure(native) == ChannelFailure::None);
    native.unsentLength = native.unsentBufferSize + 1;
    assert(channelFailure(native) == ChannelFailure::RetainedBounds);
    native.unsentLength = -1;
    assert(channelFailure(native) == ChannelFailure::RetainedBounds);
    native.unsentLength = 0; native.unsentBufferSize = -1;
    assert(channelFailure(native) == ChannelFailure::RetainedBounds);
    native.unsentBufferSize = 131072; native.fragmentLength = 1300;
    assert(channelFailure(native) == ChannelFailure::IncomingFragment);
    native.fragmentLength = -1;
    assert(channelFailure(native) == ChannelFailure::IncomingFragment);
    native.fragmentLength = 0; native.active = false;
    assert(channelFailure(native) == ChannelFailure::InactiveHuman);
    native.active = true;
    for (bool Channel::* member : {&Channel::downloading, &Channel::downloadHandle, &Channel::downloadingWWW}) {
        native.*member = true;
        assert(channelFailure(native) == ChannelFailure::Download);
        native.*member = false;
    }
    State accepted{true, true, true, true, true, true, true, false, true, true, true, true, true, 1};
    assert(failureMask(accepted) == 0);
    const struct { bool State::* member; std::uint32_t mask; } predicates[] = {
        {&State::benchmark, Benchmark}, {&State::configured, Configuration}, {&State::ready, Ready},
        {&State::fixture, Fixture}, {&State::session, Session}, {&State::running, ServerRunning},
        {&State::game, Game}, {&State::clients, Clients}, {&State::pluginsAbsent, Plugins},
        {&State::webadminAbsent, WebAdmin}, {&State::queuesQuiet, Queues}, {&State::pauseStateValid, PauseState}
    };
    for (const auto& predicate : predicates) {
        auto state = accepted; state.*predicate.member = false;
        assert(failureMask(state) == predicate.mask);
    }
    for (unsigned humans : {0u, 2u, 64u}) {
        auto state = accepted; state.humans = humans;
        assert(failureMask(state) == HumanCount);
    }
    auto state = accepted; state.inServerFrame = true;
    assert(failureMask(state) == InsideServerFrame);
    state = accepted; state.channel = ChannelFailure::OutgoingFragment;
    assert(failureMask(state) == NativeChannel);
    state.webadminAbsent = state.pluginsAbsent = false;
    assert(failureMask(state) == (NativeChannel | WebAdmin | Plugins));
    assert(channelFailureName(ChannelFailure::OutgoingFragment)[0] == 'o');
    // Production uses these exact formatters in its authenticated 160-byte
    // reply. Extreme valid integer representations must never truncate fields.
    char diagnostic[160]{};
    Channel extreme{(std::numeric_limits<int>::min)(), (std::numeric_limits<int>::min)(),
        (std::numeric_limits<int>::min)(), true, false, true, true, true};
    state.channel = ChannelFailure::IncomingFragment;
    state.humans = (std::numeric_limits<unsigned>::max)();
    int length = channelDiagnostic(diagnostic, sizeof(diagnostic), state, (std::numeric_limits<int>::min)(), extreme);
    assert(length >= 0 && length < static_cast<int>(sizeof(diagnostic)));
    assert(std::strstr(diagnostic, "extent=-2147483648"));
    assert(std::strstr(diagnostic, "dl=1/1/1 h=4294967295"));
    length = stateDiagnostic(diagnostic, sizeof(diagnostic), state,
        (std::numeric_limits<std::uint64_t>::max)(), (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(), true);
    assert(length >= 0 && length < static_cast<int>(sizeof(diagnostic)));
    assert(std::strstr(diagnostic, "s=ffffffffffffffff/ffffffffffffffff"));
    assert(std::strstr(diagnostic, "pause=1"));
    std::puts("Native retained fragment extent and owned checkpoint admission guards passed");
}
