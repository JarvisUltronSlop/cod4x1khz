#include "core.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while(0)
int main() {
    khz::DeadlineClock clock; clock.reset(17, 10000003);
    CHECK(clock.deadline() == 10018); CHECK(clock.due(10017) == 0);
    CHECK(clock.due(10018) == 1); clock.advance();
    CHECK(clock.due(100000020) == 8);
    for (unsigned i = 1; i < 1000; ++i) clock.advance();
    CHECK(clock.tick() == 1000); CHECK(clock.deadline() == 10010021);
    clock.reset(0, 10000000);
    for (unsigned i = 0; i < 600000; ++i) { CHECK(clock.due((i + 1ULL) * 10000) == 1); clock.advance(); }
    CHECK(clock.tick() == 600000);
    khz::PresentationClock presentation;
    for (unsigned cap : {125, 240, 1000}) {
        presentation.reset(10000, 10000000, cap);
        unsigned frames = 0;
        for (unsigned tick = 1; tick <= 1000; ++tick)
            if (presentation.present(tick * 10000ULL + (tick % 7) * 10, true)) ++frames;
        CHECK(frames == cap);
    }
    presentation.reset(0, 10000000, 125);
    CHECK(!presentation.present(0, false)); CHECK(presentation.present(0, true));
    CHECK(presentation.present(1000000, true)); // one presentation after a 100 ms stall
    CHECK(!presentation.present(1000000, true)); CHECK(!presentation.present(1039999, true));
    CHECK(presentation.present(1040000, true)); // phase retained at 104 ms
    presentation.reset(10000, 10000000, 0);
    clock.reset(10000, 10000000);
    CHECK(presentation.present(10100, true));
    CHECK(presentation.present(12000, true));
    CHECK(!presentation.present(14000, false));
    CHECK(presentation.present(18000, true));
    CHECK(clock.tick() == 0 && clock.debt(19000) == 0);
    CHECK(clock.deadline() == 20000); // several frames, no extra gameplay tick
    const auto wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(wakeEvent);
    const auto clockFrequency = khz::frequency();
    CHECK(SetEvent(wakeEvent));
    CHECK(!khz::waitUntilOrEvent(khz::counter(), wakeEvent)); // gameplay deadline wins
    CHECK(khz::waitUntilOrEvent(khz::counter() + clockFrequency / 4, wakeEvent));
    std::thread rendererWake([&] { Sleep(5); CHECK(SetEvent(wakeEvent)); });
    const auto rendererDeadline = khz::counter() + clockFrequency / 2;
    CHECK(khz::waitUntilOrEvent(rendererDeadline, wakeEvent));
    rendererWake.join();
    const auto resumedDeadline = khz::counter() + clockFrequency / 250;
    CHECK(!khz::waitUntilOrEvent(resumedDeadline, wakeEvent));
    CHECK(khz::counter() >= resumedDeadline); // early wake canceled its pending timer
    const auto timerDeadline = khz::counter() + clockFrequency / 1000;
    khz::waitUntil(timerDeadline);
    CHECK(khz::counter() >= timerDeadline);
    CHECK(CloseHandle(wakeEvent));
    khz::FractionalPhase idleFine, idleCoarse;
    int finePhase = 0, coarsePhase = 0;
    for (unsigned tick = 0; tick < 1000; ++tick) idleFine.advance(finePhase, double(.3f));
    for (unsigned frame = 0; frame < 125; ++frame) idleCoarse.advance(coarsePhase, double(.3f) * 8);
    CHECK(finePhase == 300 && coarsePhase == finePhase);
    finePhase = 0; idleFine.advance(finePhase, double(.3f)); CHECK(finePhase == 0);
    for (unsigned tick = 1; tick < 10; ++tick) idleFine.advance(finePhase, double(.3f));
    CHECK(finePhase == 3);
    float filtered = 0;
    const auto blend = khz::blend50ms(.5f, .001f);
    for (int tick = 0; tick < 50; ++tick) filtered += (1 - filtered) * blend;
    CHECK(std::fabs(filtered - .5f) < .000001f);
    CHECK(std::fabs(khz::blend50ms(.5f, .05f) - .5f) < .000001f);
    CHECK(khz::blend50ms(0, .001f) == 0 && khz::blend50ms(1, .001f) == 1);
    khz::DeadlineCalendar<1024> calendar;
    const auto early = calendar.keyFor(0xFFFFFFULL + 1);
    const auto late = calendar.keyFor((1ULL << 40) + 123);
    CHECK(early && late && early != late); CHECK(calendar.keyFor(0xFFFFFFULL + 1) == early);
    CHECK(calendar.takeDue(0xFFFFFFULL) == 0); CHECK(calendar.takeDue(0xFFFFFFULL + 1) == early);
    calendar.release(early); CHECK(calendar.takeDue(1ULL << 40) == 0);
    CHECK(calendar.takeDue((1ULL << 40) + 123) == late); calendar.release(late); CHECK(calendar.size() == 0);
    std::mt19937 random(123); std::vector<std::pair<khz::Tick, unsigned>> expected;
    for (unsigned i = 0; i < 1024; ++i) { auto deadline = 1000ULL + std::uint64_t(random()) * 1024 + i; auto key = calendar.keyFor(deadline); CHECK(key); expected.emplace_back(deadline, key); }
    CHECK(calendar.keyFor(1) == 0); std::sort(expected.begin(), expected.end());
    for (auto [deadline, key] : expected) { CHECK(calendar.takeDue(deadline) == key); calendar.release(key); }
    for (unsigned i = 0; i < 10000; ++i) { auto key = calendar.keyFor(i); CHECK(key); calendar.release(key); }
    CHECK(calendar.size() == 0);
    CHECK(khz::waitTicks(0.0f) == 1);
    CHECK(khz::waitTicks(.001f) == 1);
    CHECK(khz::waitTicks(.05f) == 50);
    CHECK(khz::waitTicks(.1f) == 100);
    CHECK(khz::waitTicks(.0011f) == 2);
    CHECK(khz::waitTicks(3600.0) == 3600000);
    khz::SequenceWindow<2048> received;
    CHECK(!received.accept(0));
    CHECK(received.accept(100)); CHECK(received.accept(98));
    CHECK(!received.accept(100)); CHECK(!received.accept(98));
    CHECK(received.accept(99)); CHECK(received.highest() == 100);
    CHECK(received.accept(3000)); CHECK(!received.accept(100));
    CHECK(received.accept(2999)); CHECK(!received.accept(2999));
    received.reset(); CHECK(received.accept(100));
    struct Input { unsigned buttons = 0; int movement = 0; };
    khz::InputLatch<Input> latch;
    latch.receive({1, 127}, 1); latch.receive({0, 0}, 2);
    CHECK(latch.consume().buttons == 1); CHECK(latch.consume().buttons == 0);
    khz::TickHistory<unsigned, 256> rewind;
    CHECK(!rewind.find(0)); rewind.begin(0) = 7; CHECK(*rewind.find(0) == 7);
    rewind.begin(256) = 9; CHECK(!rewind.find(0)); CHECK(*rewind.find(256) == 9);
    rewind.begin(1ULL << 40) = 11; CHECK(*rewind.find(1ULL << 40) == 11);
    rewind.reset(); CHECK(!rewind.find(1ULL << 40));
    latch.receive({1, 127}, 3);
    CHECK(latch.consume().buttons == 1); CHECK(latch.consume().buttons == 1);
    latch.receive({0, 0}, 4); CHECK(latch.consume().buttons == 0);
    latch.receive({1, -127}, 2); // late press after a newer release
    auto lateInput = latch.consume(); CHECK(lateInput.buttons == 1 && lateInput.movement == 0);
    CHECK(latch.consume().buttons == 0);
    latch.reset(); latch.receive({0, 0}, 2); latch.receive({1, 127}, 1);
    CHECK(latch.consume().buttons == 1); CHECK(latch.consume().buttons == 0);
    khz::SpscQueue<unsigned, 2048> queue;
    std::thread producer([&] { for (unsigned i = 0; i < 100000; ++i) while (!queue.push(i)) std::this_thread::yield(); });
    for (unsigned i = 0; i < 100000; ++i) { unsigned value; while (!queue.pop(value)) std::this_thread::yield(); CHECK(value == i); }
    producer.join(); CHECK(queue.size() == 0);
    struct Event { int value; std::uint64_t timestamp; };
    khz::SpscQueue<Event, 4> timed;
    CHECK(timed.push({1, 100})); CHECK(timed.push({2, 102}));
    Event event{};
    CHECK(!timed.popDue(event, 99)); CHECK(timed.size() == 2);
    CHECK(timed.popDue(event, 100) && event.value == 1);
    CHECK(!timed.popDue(event, 101)); CHECK(timed.popDue(event, 102) && event.value == 2);
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < 4; ++i) CHECK(timed.push({i, 200 + unsigned(i)}));
        CHECK(!timed.push({9, 204}));
        for (int i = 0; i < 4; ++i) CHECK(timed.popDue(event, 203) && event.value == i);
    }
    std::array<std::uint8_t, khz::WireBytes> bytes; khz::WireHeader original{123, 1ULL << 40, 500, 499}, parsed;
    khz::encode(bytes.data(), original); CHECK(khz::decode(bytes.data(), bytes.size(), parsed)); CHECK(parsed.tick == original.tick && parsed.ack == 499);
    CHECK(!khz::decode(bytes.data(), bytes.size() - 1, parsed)); bytes[0] ^= 1; CHECK(!khz::decode(bytes.data(), bytes.size(), parsed));
    CHECK(khz::validClientHeader({123, 20, 8, 19}, 123, 20));
    CHECK(!khz::validClientHeader({124, 20, 8, 19}, 123, 20));
    CHECK(!khz::validClientHeader({123, 20, 8, 21}, 123, 20));
    CHECK(!khz::validClientHeader({123, 20, 1ULL << 40, 19}, 123, 20));
    CHECK(khz::checkClientHeader({122, 100000, 8, 99999}, 123, 1) == khz::HeaderDisposition::StaleSession);
    CHECK(khz::checkClientHeader({0, 0, 0, 0}, 123, 1) == khz::HeaderDisposition::StaleSession);
    CHECK(khz::checkClientHeader({123, 20, 8, 21}, 123, 20) == khz::HeaderDisposition::Invalid);
    CHECK(khz::checkClientHeader({122, 20, 1ULL << 40, 19}, 123, 20) == khz::HeaderDisposition::Invalid);
    CHECK(khz::validServerHeader({123, 20, 7, 8}, 8));
    CHECK(!khz::validServerHeader({123, 20, 9, 8}, 9));
    CHECK(!khz::validServerHeader({123, 20, 7, 9}, 8));
    khz::Histogram histogram; for (unsigned i = 0; i < 100; ++i) histogram.add(i);
    CHECK(histogram.percentile(99) == 98); CHECK(histogram.maximum == 99);
    std::puts("PASS: absolute deadlines, renderer event wakes, uncapped presentation, debt, 600k ticks, 64-bit VM deadlines, heap cancellation, concurrent input queue, wire validation, histograms");
}
