// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Open-loop replay: the harness that makes a latency number mean something.
//
// THE PROBLEM THIS SOLVES
//
// The obvious way to measure a feed handler is a loop:
//
//     for (frame : frames) { t0 = now(); handle(frame); record(now() - t0); }
//
// That measures service time, and service time is not latency. When the system
// stalls, this loop stalls with it and stops issuing work — so a 100ms hiccup
// contributes exactly one slow sample instead of the ten thousand messages that
// were actually delayed behind it. The reported p99.9 then describes the
// harness, not the system. Gil Tene named this coordinated omission, and it is
// the single most common way a latency benchmark lies.
//
// THE FIX
//
// Replay at the recorded inter-arrival pacing, regardless of whether the
// consumer keeps up, and measure each message from the instant it was
// *supposed* to be processed rather than the instant work actually began:
//
//     latency = completion_time - scheduled_time
//
// If the handler falls behind, the backlog shows up in every subsequent
// message's latency, exactly as it would in production where the venue keeps
// sending no matter how busy you are. No correction step is needed, because
// nothing was omitted.
//
// `Histogram::record_corrected` remains available for the case where you only
// have service times after the fact and want to reconstruct the tail. Measuring
// against the schedule is strictly better; the correction is a repair.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "crossbook/histogram.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

/// One captured frame and the time it arrived, in nanoseconds since an
/// arbitrary epoch. Only differences matter.
struct ReplayEvent {
    Timestamp ts_recv{0};
    std::string_view frame;
};

struct ReplayOptions {
    /// Multiplier on the recorded timeline. 1.0 replays at the original rate;
    /// 10.0 compresses an hour into six minutes and is how you find the rate at
    /// which the handler stops keeping up.
    double speed{1.0};

    /// Discard this many events before recording, so page faults, cold caches,
    /// and branch predictor warm-up do not land in the reported tail.
    std::size_t warmup_events{0};

    /// Track how far behind schedule the replay fell. If the handler cannot
    /// keep up at the requested speed, the latency figures are still valid —
    /// they describe a system that is genuinely too slow — but you need to know
    /// that is what you measured.
    bool record_falling_behind{true};
};

/// Outcome of a replay run.
struct ReplayResult {
    LatencyReport latency;      ///< Per-message, measured against the schedule.
    std::uint64_t events{0};    ///< Events replayed after warm-up.
    std::uint64_t skipped{0};   ///< Warm-up events discarded.

    /// How often the handler was already past an event's scheduled time when it
    /// got to it. A non-zero value means the replay was not keeping up, so the
    /// tail reflects a real backlog rather than a scheduling artefact.
    std::uint64_t behind_schedule{0};

    /// Worst observed lateness, nanoseconds. If this is large, the run measured
    /// saturation, not steady-state latency.
    std::uint64_t max_lateness{0};

    /// Wall-clock nanoseconds the replay occupied.
    std::uint64_t wall_ns{0};

    /// True if the handler kept pace throughout, which is the precondition for
    /// reading these percentiles as steady-state latency.
    [[nodiscard]] bool kept_pace() const noexcept { return behind_schedule == 0; }
};

namespace detail {

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// How close to a deadline the harness stops sleeping and starts spinning.
///
/// This is 20ms, which looks absurdly large until you measure it.
///
/// `sleep_for` guarantees only that it sleeps *at least* the requested
/// duration. The actual granularity comes from the OS timer, which on Windows
/// defaults to 15.6ms. Asking for 3ms can therefore return after 15ms or more.
///
/// Both smaller values were tried and both failed on a stock Windows desktop,
/// with an empty handler and nothing else to blame:
///
///   200us margin, then yield()  -> 37 of 40 events missed their deadline
///   2ms margin                  -> 8 of 20 missed, max lateness 5.3ms
///
/// The margin has to exceed the platform's worst-case timer granularity or the
/// harness spends its time measuring its own scheduler. At 20ms it does.
///
/// Everything inside this window is busy-waited, so a replay of a feed with
/// sub-20ms gaps spins continuously and pins a core. That is the correct trade
/// for a measurement tool, and it is what latency harnesses generally do: a
/// pacing error of several milliseconds is enormous next to the sub-microsecond
/// latencies being measured, and would swamp every number in the report.
/// Quiet periods longer than 20ms still sleep, so an idle overnight capture
/// does not burn CPU for nothing.
inline constexpr std::uint64_t kSpinThresholdNs = 20'000'000;  // 20 ms

/// Wait until `target_ns`. Sleeps while far away to avoid burning a core for no
/// reason, then busy-waits the final stretch so the deadline is actually hit.
inline void wait_until(std::uint64_t target_ns) noexcept {
    for (;;) {
        const std::uint64_t now = now_ns();
        if (now >= target_ns) {
            return;
        }
        const std::uint64_t remaining = target_ns - now;
        if (remaining > kSpinThresholdNs) {
            // Leave the full spin margin: sleep_for overshoots, never undershoots.
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(remaining - kSpinThresholdNs));
        }
        // Otherwise fall through and spin. No yield(): handing the core away
        // invites the scheduler not to give it back until after the deadline,
        // which is the problem being avoided.
    }
}

}  // namespace detail

/// Replay `events` through `handler`, paced open-loop.
///
/// `handler` is invoked as `handler(const ReplayEvent&)`; its return value is
/// ignored. Latency is measured from each event's scheduled time to the moment
/// the handler returns, so queueing delay is included by construction.
template <typename Handler>
[[nodiscard]] ReplayResult replay_open_loop(const std::vector<ReplayEvent>& events,
                                            Handler&& handler,
                                            const ReplayOptions& options = {}) {
    ReplayResult result;
    if (events.empty()) {
        return result;
    }

    const double speed = (options.speed > 0.0) ? options.speed : 1.0;
    Histogram histogram;

    // The schedule starts slightly in the future.
    //
    // Without the lead-in, event 0's deadline is `wall_start` itself, and the
    // clock read a few hundred nanoseconds later is already past it — so
    // event 0 counted as late on every run and `kept_pace()` was a constant
    // false. That is the gate documented as "the precondition for reading
    // these percentiles as steady-state latency", so it conveyed nothing, and
    // `max_lateness` carried a sub-microsecond artifact that had nothing to do
    // with the handler. One millisecond is far below any real inter-arrival
    // gap and far above the cost of entering the loop.
    static constexpr std::uint64_t kScheduleLeadNs = 1'000'000;
    const std::uint64_t wall_start = detail::now_ns() + kScheduleLeadNs;
    const Timestamp epoch = events.front().ts_recv;

    for (std::size_t i = 0; i < events.size(); ++i) {
        const ReplayEvent& event = events[i];

        // Where this event belongs on the wall clock. Derived from the ORIGINAL
        // recorded timeline, never from when the previous one happened to
        // finish — that is the whole point.
        const auto offset = static_cast<double>(event.ts_recv - epoch) / speed;
        const std::uint64_t scheduled = wall_start + static_cast<std::uint64_t>(offset);

        const std::uint64_t before_wait = detail::now_ns();
        if (before_wait < scheduled) {
            detail::wait_until(scheduled);
        } else if (options.record_falling_behind && i >= options.warmup_events) {
            // Already past the deadline: the handler is not keeping up.
            ++result.behind_schedule;
            const std::uint64_t lateness = before_wait - scheduled;
            if (lateness > result.max_lateness) {
                result.max_lateness = lateness;
            }
        }

        handler(event);

        if (i < options.warmup_events) {
            ++result.skipped;
            continue;
        }

        // Completion measured against the SCHEDULE, not against the start of
        // work. A message that waited behind a backlog reports the wait.
        const std::uint64_t completed = detail::now_ns();
        histogram.record(completed > scheduled ? completed - scheduled : 0);
        ++result.events;
    }

    result.wall_ns = detail::now_ns() - wall_start;
    result.latency = summarise(histogram);
    return result;
}

/// Replay as fast as the machine allows, ignoring the recorded timeline.
///
/// This measures THROUGHPUT. The per-event figure it returns is a mean service
/// time and must never be presented as latency: with no pacing there is no
/// schedule to be late against, so the tail is meaningless by construction.
/// Useful for answering "how much headroom is there", and for nothing else.
struct ThroughputResult {
    std::uint64_t events{0};
    std::uint64_t wall_ns{0};

    /// True when the run took long enough for the clock to resolve it.
    ///
    /// A trivial handler over a short capture can finish inside the clock's
    /// tick, leaving `wall_ns` at zero. The rate is then genuinely unknown, and
    /// callers must not read the 0.0 below as "no throughput" — that is the
    /// division guard, not a measurement. Lengthen the capture instead.
    [[nodiscard]] bool measurable() const noexcept { return wall_ns > 0; }

    /// Events per second, or 0.0 when `measurable()` is false.
    [[nodiscard]] double events_per_second() const noexcept {
        return wall_ns == 0 ? 0.0
                            : static_cast<double>(events) * 1e9 / static_cast<double>(wall_ns);
    }

    /// Mean service time, or 0.0 when nothing ran. Never a latency figure: with
    /// no pacing there is no schedule to be late against.
    [[nodiscard]] double mean_ns() const noexcept {
        return events == 0 ? 0.0 : static_cast<double>(wall_ns) / static_cast<double>(events);
    }
};

template <typename Handler>
[[nodiscard]] ThroughputResult replay_throughput(const std::vector<ReplayEvent>& events,
                                                 Handler&& handler) {
    const std::uint64_t start = detail::now_ns();
    for (const ReplayEvent& event : events) {
        handler(event);
    }
    ThroughputResult result;
    result.wall_ns = detail::now_ns() - start;
    result.events = events.size();
    return result;
}

/// Median inter-arrival gap across a capture, in nanoseconds.
///
/// The natural `expected_interval` for `Histogram::record_corrected` when
/// repairing service-time measurements after the fact. Median rather than mean,
/// because market data arrives in bursts and the mean is dragged around by
/// quiet periods.
[[nodiscard]] inline std::uint64_t median_interval_ns(const std::vector<ReplayEvent>& events) {
    if (events.size() < 2) {
        return 0;
    }
    std::vector<std::uint64_t> gaps;
    gaps.reserve(events.size() - 1);
    for (std::size_t i = 1; i < events.size(); ++i) {
        const Timestamp delta = events[i].ts_recv - events[i - 1].ts_recv;
        gaps.push_back(delta > 0 ? static_cast<std::uint64_t>(delta) : 0);
    }
    const std::size_t mid = gaps.size() / 2;
    std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(mid), gaps.end());
    return gaps[mid];
}

}  // namespace crossbook
