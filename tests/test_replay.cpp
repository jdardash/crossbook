// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Tests for the open-loop replay harness.
//
// These are deliberately short-running and tolerant of scheduler noise: they
// assert the harness's *semantics* (does it pace, does it measure against the
// schedule, does it notice falling behind), not wall-clock precision, which no
// test on a shared CI runner can assert honestly.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "crossbook/replay.hpp"

using namespace crossbook;

namespace {

/// A capture with events spaced `interval_ns` apart.
std::vector<ReplayEvent> make_capture(std::size_t count, Timestamp interval_ns,
                                      std::string_view frame = "{}") {
    std::vector<ReplayEvent> events;
    events.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        events.push_back(ReplayEvent{static_cast<Timestamp>(i) * interval_ns, frame});
    }
    return events;
}

}  // namespace

TEST_CASE("replaying an empty capture is safe", "[replay]") {
    const ReplayResult r = replay_open_loop({}, [](const ReplayEvent&) {});
    CHECK(r.events == 0);
    CHECK(r.latency.count == 0);
}

TEST_CASE("every event reaches the handler exactly once", "[replay]") {
    const auto events = make_capture(200, 10'000);  // 10us apart
    std::uint64_t seen = 0;
    const ReplayResult r = replay_open_loop(events, [&](const ReplayEvent&) { ++seen; });
    CHECK(seen == 200);
    CHECK(r.events == 200);
    CHECK(r.latency.count == 200);
}

TEST_CASE("warm-up events run but are excluded from the report", "[replay]") {
    // Page faults and cold caches belong to the warm-up, not the tail.
    const auto events = make_capture(100, 10'000);
    std::uint64_t handled = 0;

    ReplayOptions options;
    options.warmup_events = 20;
    const ReplayResult r = replay_open_loop(events, [&](const ReplayEvent&) { ++handled; },
                                            options);

    CHECK(handled == 100);   // All of them were processed...
    CHECK(r.skipped == 20);  // ...but the first 20 were not measured.
    CHECK(r.events == 80);
    CHECK(r.latency.count == 80);
}

TEST_CASE("replay is paced by the recorded timeline", "[replay][timing]") {
    // 50 events at 1ms apart should take roughly 50ms, not zero. The bound is
    // deliberately loose: this asserts that pacing happens at all, which is the
    // property, rather than asserting timer precision on a shared runner.
    const auto events = make_capture(50, 1'000'000);  // 1ms apart

    const auto start = std::chrono::steady_clock::now();
    const ReplayResult r = replay_open_loop(events, [](const ReplayEvent&) {});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    CHECK(r.events == 50);
    CHECK(elapsed_ms >= 30);  // Paced, not instantaneous.
}

TEST_CASE("speed compresses the timeline", "[replay][timing]") {
    // The knob for finding the rate at which a handler stops keeping up.
    //
    // The capture spans 100ms at 1x; at 50x it should take about 2ms. The bound
    // below sits far above that and far below the uncompressed span, so it only
    // fails if compression is not happening at all. Tighter would be asserting
    // the CI runner's scheduling latency rather than this library's behaviour.
    const auto events = make_capture(20, 5'000'000);  // 5ms apart = 100ms at 1x

    ReplayOptions fast;
    fast.speed = 50.0;

    const auto start = std::chrono::steady_clock::now();
    const ReplayResult r = replay_open_loop(events, [](const ReplayEvent&) {}, fast);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();

    CHECK(r.events == 20);
    CHECK(elapsed_ms < 60);  // Far shorter than the 50ms the capture spans.
}

TEST_CASE("a fast handler keeps pace", "[replay][timing]") {
    // An empty handler against 1ms spacing has four orders of magnitude of
    // slack, so any lateness here is the harness failing to hit its own
    // deadlines rather than the handler being slow. That is exactly the bug an
    // earlier version had: sleeping to within 200us of the deadline and
    // trusting the OS timer, which missed 37 of 40.
    //
    // The threshold is not zero because CI runners are shared and preemption is
    // real; a handful of late events on a contended box is honest. A third of
    // them is a broken harness.
    // 5ms spacing rather than 1ms so that being late requires a preemption
    // longer than 5ms. That makes the assertion both tighter and less
    // dependent on how busy the machine is.
    const auto events = make_capture(20, 5'000'000);
    const ReplayResult r = replay_open_loop(events, [](const ReplayEvent&) {});
    INFO("behind_schedule=" << r.behind_schedule << " max_lateness=" << r.max_lateness);
    CHECK(r.behind_schedule <= 2);
}

// ---------------------------------------------------------------------------
// The property this harness exists for
// ---------------------------------------------------------------------------

TEST_CASE("a stalled handler shows up in later events' latency", "[replay][omission]") {
    // THE COORDINATED OMISSION TEST.
    //
    // A closed loop that timed only handler execution would report one slow
    // sample and call everything after it fast, because it would not have sent
    // anything during the stall. Measuring against the schedule means the
    // backlog lands on every message queued behind the stall, which is what
    // production would experience.
    //
    // 30 events, 1ms apart. Event 5 blocks for 20ms — long enough that events
    // 6 through ~25 were all due while the handler was busy.

    const auto events = make_capture(30, 1'000'000);
    std::size_t index = 0;

    const ReplayResult r = replay_open_loop(events, [&](const ReplayEvent&) {
        if (index == 5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ++index;
    });

    CHECK(r.events == 30);

    // The handler fell behind, and the harness noticed rather than silently
    // absorbing it.
    CHECK(r.behind_schedule > 0);
    CHECK_FALSE(r.kept_pace());
    CHECK(r.max_lateness > 1'000'000);  // More than one inter-arrival gap.

    // Crucially, the delay is not confined to a single sample. If only one
    // event were affected, p50 would be untouched — that is precisely the
    // optimistic answer a closed loop gives.
    INFO("p50=" << r.latency.p50 << " p99=" << r.latency.p99 << " max=" << r.latency.max);
    CHECK(r.latency.max > 5'000'000);   // The stall itself.
    CHECK(r.latency.p50 > 100'000);     // And the queue behind it.
}

TEST_CASE("throughput replay is labelled as throughput, not latency", "[replay]") {
    // No pacing means no schedule, so there is nothing to be late against and
    // the distribution is meaningless. The API returns a mean and a rate, and
    // deliberately offers no percentiles at all.
    const auto events = make_capture(10'000, 1'000);
    std::uint64_t seen = 0;
    const ThroughputResult r = replay_throughput(events, [&](const ReplayEvent&) { ++seen; });

    CHECK(seen == 10'000);
    CHECK(r.events == 10'000);
    CHECK(r.events_per_second() > 0.0);
    CHECK(r.mean_ns() >= 0.0);
}

TEST_CASE("median_interval_ns finds the typical gap despite bursts", "[replay]") {
    // Market data arrives in bursts, so the mean gap is dragged around by quiet
    // periods. The median is what a correction step should use.
    std::vector<ReplayEvent> events;
    Timestamp t = 0;
    for (int i = 0; i < 100; ++i) {
        events.push_back(ReplayEvent{t, "{}"});
        t += 1'000;  // 1us apart
    }
    t += 10'000'000'000LL;  // One 10-second silence.
    events.push_back(ReplayEvent{t, "{}"});

    CHECK(median_interval_ns(events) == 1'000);
}

TEST_CASE("median_interval_ns handles degenerate captures", "[replay]") {
    CHECK(median_interval_ns({}) == 0);
    CHECK(median_interval_ns(make_capture(1, 1000)) == 0);
}
