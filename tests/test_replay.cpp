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

/// Can this machine hold a millisecond deadline right now?
///
/// The pacing tests below assert a property of the HARNESS — that it hits the
/// deadlines it sets itself. On a quiet machine that is a clean assertion. On a
/// machine that is compiling in another window, the OS will preempt the replay
/// for longer than the interval being measured, and the test fails for a reason
/// that has nothing to do with the code under test.
///
/// That happened: a run that shared a core with a full rebuild failed one
/// timing assertion, and ten subsequent runs passed. Loosening the bound until
/// it never fails would make the test assert nothing. Detecting the condition
/// and skipping is honest — the property is simply not measurable here.
[[nodiscard]] bool machine_can_keep_time() {
    // Deliberately NOT built on replay_open_loop. Using the harness as its own
    // contention detector would mean a genuinely broken harness fails the probe,
    // skips the test, and reports green — which is worse than the flake it was
    // meant to fix.
    //
    // So this is an independent spin loop, and it demands exactly what the test
    // below demands: twenty consecutive 5ms deadlines, none missed by more than
    // 1ms. A first attempt used 2ms deadlines and tolerated three misses in ten;
    // it passed while three capture processes were saturating the machine, and
    // the real test then failed anyway. A probe looser than the assertion it
    // guards is not a guard.
    constexpr int kProbes = 20;
    constexpr auto kInterval = std::chrono::milliseconds(5);
    constexpr auto kTolerance = std::chrono::milliseconds(1);

    for (int i = 0; i < kProbes; ++i) {
        const auto target = std::chrono::steady_clock::now() + kInterval;
        while (std::chrono::steady_clock::now() < target) {
            // Spin, exactly as the harness does inside its threshold.
        }
        if (std::chrono::steady_clock::now() - target > kTolerance) {
            return false;
        }
    }
    return true;
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
    if (!machine_can_keep_time()) {
        SKIP("machine is too loaded to measure pacing");
    }
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
    if (!machine_can_keep_time()) {
        SKIP("machine is too loaded to measure pacing");
    }
    // The knob for finding the rate at which a handler stops keeping up.
    //
    // The capture spans 100ms at 1x; at 50x it should take about 2ms. The bound
    // below sits far above that and far below the uncompressed span, so it only
    // fails if compression is not happening at all. Tighter would be asserting
    // the CI runner's scheduling latency rather than this library's behaviour.
    const auto events = make_capture(20, 5'000'000);  // 5ms apart = 100ms at 1x

    ReplayOptions fast;
    fast.speed = 50.0;

    // Best of three, for the same reason as the pacing test: a preemption
    // between the guard and the measurement would otherwise fail a run that
    // says nothing about compression.
    std::int64_t elapsed_ms = 0;
    ReplayResult r{};
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto start = std::chrono::steady_clock::now();
        r = replay_open_loop(events, [](const ReplayEvent&) {}, fast);
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
        if (elapsed_ms < 60) {
            break;
        }
    }

    CHECK(r.events == 20);
    CHECK(elapsed_ms < 60);  // Far shorter than the 100ms the capture spans.
}

TEST_CASE("a fast handler keeps pace", "[replay][timing]") {
    if (!machine_can_keep_time()) {
        SKIP("machine is too loaded to measure pacing");
    }
    // An empty handler with 5ms of slack per event has four orders of magnitude
    // of headroom, so any lateness is the harness failing to hit deadlines it
    // set itself. That was a real bug: an earlier version slept to within 200us
    // of the deadline and trusted the OS timer, missing 37 of 40 on Windows,
    // where the default timer granularity is 15.6ms.
    const auto events = make_capture(20, 5'000'000);
    const ReplayResult r = replay_open_loop(events, [](const ReplayEvent&) {});
    INFO("behind_schedule=" << r.behind_schedule << " max_lateness=" << r.max_lateness);
    CHECK(r.kept_pace());
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

    // A no-op handler can finish the whole capture inside one clock tick, which
    // macOS demonstrated by failing an unconditional `> 0.0` here. When the run
    // is below the clock's resolution the rate is unknown, not zero, and the
    // API says so via measurable() rather than pretending otherwise.
    if (r.measurable()) {
        CHECK(r.events_per_second() > 0.0);
        CHECK(r.mean_ns() > 0.0);
    } else {
        CHECK(r.events_per_second() == 0.0);
    }
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

TEST_CASE("sub-microsecond overshoot is not counted as falling behind",
          "[replay][omission]") {
    // The metric has to distinguish "the handler could not keep up" from "the
    // clock read that noticed the deadline cost 100ns".
    //
    // Without the tolerance, a 5ms schedule on an idle machine reported one
    // event behind by 100 nanoseconds, and every consumer of kept_pace() —
    // including the warning crossbook_verify prints next to its percentiles —
    // would have called that a saturated system.
    const auto events = make_capture(30, 2'000'000);  // 2ms apart: huge slack
    const ReplayResult r = replay_open_loop(events, [](const ReplayEvent&) {});

    INFO("behind_schedule=" << r.behind_schedule << " max_lateness=" << r.max_lateness);
    // Any lateness that IS reported must be real, i.e. above the tolerance.
    CHECK((r.behind_schedule == 0 || r.max_lateness > kDeadlineToleranceNs));
}

TEST_CASE("real lateness is still reported", "[replay][omission]") {
    // The tolerance must not swallow genuine saturation. A handler that blocks
    // for 20ms against a 1ms schedule is late by four orders of magnitude more
    // than the tolerance.
    const auto events = make_capture(30, 1'000'000);
    std::size_t index = 0;
    const ReplayResult r = replay_open_loop(events, [&](const ReplayEvent&) {
        if (index++ == 5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    CHECK(r.behind_schedule > 0);
    CHECK(r.max_lateness > kDeadlineToleranceNs * 100);
}
