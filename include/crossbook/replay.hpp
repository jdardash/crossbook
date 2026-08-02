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

/// Overshoot below this is not lateness.
///
/// A spin loop can only observe that a deadline has passed *after* it has
/// passed, so every wait overshoots by at least the cost of one clock read.
/// Counting that as falling behind makes the metric report saturation on an
/// idle machine: a 5ms schedule measured here routinely showed one event
/// "behind" by 100 NANOSECONDS, which is noise in the deadline check, not a
/// handler that could not keep up.
///
/// One microsecond is comfortably above clock-read cost on every platform in
/// the CI matrix and three orders of magnitude below any inter-arrival gap
/// worth pacing, so nothing operationally meaningful hides under it.
inline constexpr std::uint64_t kDeadlineToleranceNs = 1'000;

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

    const std::uint64_t wall_start = detail::now_ns();
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
            // Past the deadline. Only count it if the overshoot exceeds the
            // granularity of noticing — see kDeadlineToleranceNs.
            const std::uint64_t lateness = before_wait - scheduled;
            if (lateness > kDeadlineToleranceNs) {
                ++result.behind_schedule;
                if (lateness > result.max_lateness) {
                    result.max_lateness = lateness;
                }
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

// ---------------------------------------------------------------------------
// Rate-vs-latency sweep
// ---------------------------------------------------------------------------
//
// WHY A SWEEP AND NOT A SINGLE RUN
//
// One replay at one speed answers one question: "what is the latency at the
// recorded rate". The question a capacity decision actually needs answered is
// "at what offered rate does the latency stop being flat" — and a single point
// cannot show that. The measurement practitioners trust (STAC-M1 and its
// descendants) is the whole curve: latency versus offered rate, flat while the
// system has headroom, diverging as the rate approaches saturation. The
// artifact is the curve plus the knee — the highest swept rate at which the
// consumer still kept pace with p99 under a stated bound.
//
// There is also an auditing reason. `ReplayOptions::speed` existed for a long
// time with nothing driving it across a range, which made it a control that
// could never fail: a speed knob nobody turns is indistinguishable from a
// broken one. The sweep is the code that turns it.
//
// SAMPLE HONESTY
//
// A p99.9 is a statement about the 1-in-1000 event. With fewer than ~10,000
// samples that estimate rests on a handful of observations and is mostly
// noise wearing three decimal places. The sweep therefore tiles the capture
// per rate until it has `min_samples` measured events or the per-rate time
// budget is spent, and always reports n — the reader judges the tail's
// credibility from the count instead of being asked to trust it.

/// Fewest samples at which a p99.9 stops being an anecdote: the 1-in-1000
/// event has been seen roughly ten times. Rows below this are reported anyway
/// — with their n, so nobody mistakes them for a resolved tail.
inline constexpr std::uint64_t kHonestTailSamples = 10'000;

struct SweepOptions {
    /// Speed multipliers to sweep, in the order they run. Empty means the
    /// default ladder {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000}: roughly
    /// log-spaced, because the knee is found by ratio, not by increment.
    /// Non-positive entries are skipped. Ascending order is what makes early
    /// stopping meaningful.
    std::vector<double> speeds{};

    /// Measured samples to aim for at each rate; the capture is tiled
    /// (replayed back to back on a spliced timeline) until this many events
    /// will be recorded. See kHonestTailSamples for why the default is 10k.
    std::uint64_t min_samples{kHonestTailSamples};

    /// Scheduled nanoseconds each rate may occupy. Without this, a 1x rung
    /// over a 40-minute capture takes 40 minutes and the sweep is a tool
    /// nobody runs. When the budget bites, the run is truncated to the prefix
    /// of the (tiled) timeline that fits — still real contiguous traffic,
    /// still correctly paced — and the smaller n is reported, not hidden.
    std::uint64_t budget_ns_per_rate{10'000'000'000ULL};  // 10 s

    /// The knee is the highest swept rate that kept pace AND held p99 at or
    /// under this bound. The bound is part of the claim ("sustained X msg/s
    /// with p99 under Y") and must be stated next to the result; a knee with
    /// no bound attached is just the point where the harness gave up.
    std::uint64_t p99_bound_ns{100'000};  // 100 us

    /// Warm-up events discarded at the start of each rate's run, exactly as
    /// in ReplayOptions: cold caches belong to the warm-up, not the tail.
    std::size_t warmup_events{0};

    /// Stop the ladder once a rate's worst lateness exceeds this. Past that
    /// point the consumer is not merely late, it is drowning — every faster
    /// rung would measure a deeper backlog of the same failure, and the time
    /// is better spent not measuring it. The failing rung itself is kept: the
    /// sweep needs at least one point past the knee to bracket it.
    std::uint64_t stop_lateness_ns{100'000'000};  // 100 ms
};

/// One rung of the ladder: everything needed to plot the curve and audit it.
struct SweepPoint {
    double speed{1.0};          ///< Multiplier this rung ran at.
    std::uint64_t loops{1};     ///< Times the capture was tiled to reach n.

    /// Messages per second the SCHEDULE demanded. This is the x-axis of the
    /// curve, derived from the recorded timeline and the multiplier — never
    /// from how fast the run happened to go.
    double offered_rate{0.0};

    /// Messages per second actually completed, wall-clock. When the consumer
    /// keeps pace this tracks offered_rate by construction (the harness
    /// waits); a shortfall here is saturation made visible.
    double achieved_rate{0.0};

    /// The full per-rate replay result. n is result.events; the percentiles
    /// are in result.latency; kept_pace() is the per-rung verdict.
    ReplayResult result{};

    /// Whether n is large enough for the p99.9 to mean anything.
    [[nodiscard]] bool tail_honest() const noexcept {
        return result.events >= kHonestTailSamples;
    }

    /// Did this rung sustain the rate? Requires something to have actually
    /// been measured: a truncated run with zero samples must not pass on the
    /// strength of its all-zero percentiles.
    [[nodiscard]] bool passed(std::uint64_t p99_bound_ns) const noexcept {
        return result.events > 0 && result.kept_pace() && result.latency.p99 <= p99_bound_ns;
    }
};

struct SweepResult {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    std::vector<SweepPoint> points;

    /// Index of the knee: the last rung of the ladder's PASSING PREFIX — every
    /// rung up to and including it passed, and the next rung (if any) did not.
    /// Prefix rather than "last passing anywhere" on purpose: a rung that
    /// fails followed by one that passes is scheduler noise, and promoting the
    /// later pass to a sustained-rate claim would report the noise as
    /// capacity. npos when even the first rung failed.
    std::size_t knee{npos};

    /// True when the ladder was cut short by stop_lateness_ns.
    bool stopped_early{false};

    /// The bound the knee was judged against, restated here so a report can
    /// print the claim and its condition together.
    std::uint64_t p99_bound_ns{0};

    [[nodiscard]] bool has_knee() const noexcept { return knee != npos; }
};

namespace detail {

/// Concatenate `loops` copies of the capture on one continuous timeline.
///
/// The seam between copies is spliced with the capture's median inter-arrival
/// gap: a zero-gap seam would inject an artificial burst at every boundary and
/// a large one an artificial pause, and either would land in the histogram as
/// traffic the venue never sent. `stride` is the copy-to-copy timestamp shift
/// (span + splice gap), computed by the caller so the offered-rate arithmetic
/// and the tiling agree on it by construction.
[[nodiscard]] inline std::vector<ReplayEvent> tile_capture(
    const std::vector<ReplayEvent>& events, std::uint64_t loops, Timestamp stride) {
    std::vector<ReplayEvent> tiled;
    if (events.empty() || loops == 0) {
        return tiled;
    }
    tiled.reserve(events.size() * static_cast<std::size_t>(loops));
    for (std::uint64_t k = 0; k < loops; ++k) {
        const Timestamp shift = static_cast<Timestamp>(k) * stride;
        for (const ReplayEvent& e : events) {
            tiled.push_back(ReplayEvent{e.ts_recv + shift, e.frame});
        }
    }
    return tiled;
}

}  // namespace detail

/// Run `replay_open_loop` once per speed multiplier and assemble the
/// rate-vs-latency curve.
///
/// `handler` is the same callable across every rung, invoked per event as in
/// `replay_open_loop`. Any state it carries (a book, a feed) persists across
/// rungs; a capture that begins with a snapshot re-syncs it at each tiled
/// copy, which is exactly what reconnecting to the venue would do.
///
/// A capture needs at least two events and a positive span to define a rate;
/// anything less returns an empty result rather than a curve with a made-up
/// x-axis.
template <typename Handler>
[[nodiscard]] SweepResult replay_sweep(const std::vector<ReplayEvent>& events,
                                       Handler&& handler,
                                       const SweepOptions& options = {}) {
    SweepResult sweep;
    sweep.p99_bound_ns = options.p99_bound_ns;
    if (events.size() < 2) {
        return sweep;
    }
    const Timestamp epoch = events.front().ts_recv;
    const Timestamp span = events.back().ts_recv - epoch;
    if (span <= 0) {
        return sweep;  // No timeline, no rate.
    }

    // Splice gap for the tiling seam; 1ns floor so the stride always advances
    // even for a capture whose median gap rounds to zero.
    std::uint64_t gap = median_interval_ns(events);
    if (gap == 0) {
        gap = 1;
    }
    const Timestamp stride = span + static_cast<Timestamp>(gap);

    std::vector<double> speeds = options.speeds;
    if (speeds.empty()) {
        speeds = {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
    }

    const auto per_loop = static_cast<std::uint64_t>(events.size());
    const std::uint64_t wanted =
        options.min_samples + static_cast<std::uint64_t>(options.warmup_events);

    for (const double speed : speeds) {
        if (speed <= 0.0) {
            continue;  // Not a rate; skipping beats inventing a schedule for it.
        }

        // Loops needed to reach the sample target...
        std::uint64_t loops = (wanted + per_loop - 1) / per_loop;
        if (loops == 0) {
            loops = 1;
        }

        // ...capped by the time budget, expressed on the recorded timeline
        // (budget wall-nanoseconds cover `budget * speed` recorded
        // nanoseconds). Guard the product against overflow before trusting it;
        // a saturated cutoff just means "no cap".
        const double budget_span_d = static_cast<double>(options.budget_ns_per_rate) * speed;
        const bool budget_caps = budget_span_d > 0.0 && budget_span_d < 9.0e18;
        if (budget_caps) {
            const std::uint64_t max_loops =
                static_cast<std::uint64_t>(budget_span_d / static_cast<double>(stride)) + 1;
            loops = (std::min)(loops, max_loops);
        }

        std::vector<ReplayEvent> tiled = detail::tile_capture(events, loops, stride);

        // Truncate to the prefix that fits the budget. The result is still a
        // real contiguous slice of paced traffic — only shorter, and the
        // shortfall shows up in n rather than being hidden. Never cut below
        // two events: one event has no rate.
        if (budget_caps) {
            const auto cutoff = epoch + static_cast<Timestamp>(budget_span_d);
            auto keep_end = std::partition_point(
                tiled.begin(), tiled.end(),
                [&](const ReplayEvent& e) { return e.ts_recv <= cutoff; });
            if (keep_end - tiled.begin() < 2) {
                keep_end = tiled.begin() + 2;
            }
            tiled.erase(keep_end, tiled.end());
        }

        ReplayOptions replay_options;
        replay_options.speed = speed;
        // Warm-up must never eat the run. When the budget truncates a slow
        // rung to a few hundred events, a fixed warm-up sized for the full
        // capture can exceed what is left, and the rung would then report
        // n = 0 — a row that measures nothing while looking deliberate.
        // Capping warm-up at a quarter of the run keeps the intent (cold
        // caches stay out of the tail) while guaranteeing the rung measures
        // at least three quarters of what it paced.
        replay_options.warmup_events =
            (std::min)(options.warmup_events, tiled.size() / 4);

        SweepPoint point;
        point.speed = speed;
        point.loops = loops;
        point.result = replay_open_loop(tiled, handler, replay_options);

        // Offered rate from the schedule: (n-1) inter-arrival intervals over
        // the tiled span, compressed by the multiplier. Uses what was actually
        // scheduled after any truncation, so the x-axis never claims traffic
        // that was not offered.
        const Timestamp tiled_span = tiled.back().ts_recv - tiled.front().ts_recv;
        if (tiled_span > 0) {
            point.offered_rate = static_cast<double>(tiled.size() - 1) * speed * 1e9 /
                                 static_cast<double>(tiled_span);
        }
        // Achieved rate from the wall: every message processed (warm-up
        // included — it was work) over the time the run really took.
        if (point.result.wall_ns > 0) {
            point.achieved_rate =
                static_cast<double>(point.result.events + point.result.skipped) * 1e9 /
                static_cast<double>(point.result.wall_ns);
        }

        const bool drowning = point.result.max_lateness > options.stop_lateness_ns;
        sweep.points.push_back(std::move(point));
        if (drowning) {
            sweep.stopped_early = true;
            break;
        }
    }

    // The knee: end of the passing prefix. See SweepResult::knee for why a
    // pass after a failure does not count.
    for (std::size_t i = 0; i < sweep.points.size(); ++i) {
        if (!sweep.points[i].passed(options.p99_bound_ns)) {
            break;
        }
        sweep.knee = i;
    }
    return sweep;
}

}  // namespace crossbook
