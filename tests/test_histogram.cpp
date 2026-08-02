// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "crossbook/histogram.hpp"

using namespace crossbook;

TEST_CASE("an empty histogram reports nothing rather than lying", "[histogram]") {
    Histogram h;
    CHECK(h.count() == 0);
    CHECK(h.min() == 0);
    CHECK(h.max() == 0);
    CHECK(h.percentile(50.0) == 0);
    CHECK(h.mean() == 0.0);
}

TEST_CASE("single value lands on every percentile", "[histogram]") {
    Histogram h;
    h.record(1000);
    CHECK(h.count() == 1);
    CHECK(h.min() == 1000);
    CHECK(h.max() == 1000);
    for (double p : {0.0, 50.0, 90.0, 99.0, 99.99, 100.0}) {
        INFO("p=" << p);
        CHECK(h.percentile(p) >= 1000);
        CHECK(h.percentile(p) <= h.highest_equivalent_value(1000));
    }
}

TEST_CASE("percentiles are accurate to the configured precision", "[histogram]") {
    // 3 significant figures means a reported value is within 0.1% of the true
    // one. Anything looser and a p99 comparison between two runs is noise.
    Histogram h(3'600'000'000'000ULL, 3);
    for (std::uint64_t v = 1; v <= 100'000; ++v) {
        h.record(v);
    }
    CHECK(h.count() == 100'000);
    CHECK(h.min() == 1);
    CHECK(h.max() == 100'000);

    struct Case {
        double p;
        std::uint64_t expected;
    };
    for (const Case& c : {Case{50.0, 50'000}, Case{90.0, 90'000}, Case{99.0, 99'000},
                          Case{99.9, 99'900}}) {
        const std::uint64_t got = h.percentile(c.p);
        const double error = static_cast<double>(got > c.expected ? got - c.expected
                                                                  : c.expected - got) /
                             static_cast<double>(c.expected);
        INFO("p" << c.p << " expected~" << c.expected << " got " << got);
        CHECK(error < 0.001);
    }
}

TEST_CASE("percentiles are monotonic", "[histogram]") {
    // A p99 below a p90 would make every published table nonsense.
    Histogram h;
    for (std::uint64_t v = 1; v <= 10'000; ++v) {
        h.record(v * 7);
    }
    CHECK(h.percentile(50.0) <= h.percentile(90.0));
    CHECK(h.percentile(90.0) <= h.percentile(99.0));
    CHECK(h.percentile(99.0) <= h.percentile(99.9));
    CHECK(h.percentile(99.9) <= h.percentile(99.99));
    CHECK(h.percentile(99.99) <= h.max());
}

TEST_CASE("no percentile ever exceeds the observed maximum", "[histogram]") {
    // Bucket upper bounds can sit above anything actually recorded. Reporting
    // one as a percentile produces a table where p99.99 > max, which reads as a
    // bug even though the bound is technically valid.
    for (std::uint64_t top : {97ULL, 1000ULL, 100'000ULL, 7'654'321ULL}) {
        Histogram h;
        for (std::uint64_t v = 1; v <= top; ++v) {
            h.record(v);
        }
        INFO("top=" << top << " max=" << h.max());
        CHECK(h.max() == top);
        CHECK(h.percentile(100.0) == h.max());
        CHECK(h.percentile(99.99) <= h.max());
        CHECK(h.percentile(50.0) <= h.max());
    }
}

TEST_CASE("values span nanoseconds to an hour", "[histogram]") {
    // The whole point of log-linear bucketing: constant relative precision over
    // a range no linear histogram could cover.
    Histogram h;
    for (std::uint64_t v : {1ULL, 1'000ULL, 1'000'000ULL, 1'000'000'000ULL, 60'000'000'000ULL}) {
        h.record(v);
    }
    CHECK(h.count() == 5);
    CHECK(h.min() == 1);
    CHECK(h.max() >= 60'000'000'000ULL);
    CHECK(h.overflow_count() == 0);
}

TEST_CASE("values beyond the range are clamped and counted, not dropped", "[histogram]") {
    // Silently discarding the largest observations would make the tail look
    // better the worse the system behaved.
    Histogram h(1'000'000, 3);
    h.record(500);
    h.record(999'999'999);
    CHECK(h.count() == 2);
    CHECK(h.overflow_count() == 1);
    CHECK(h.max() <= h.highest_equivalent_value(1'000'000));
}

TEST_CASE("equivalent-value bounds bracket the input", "[histogram]") {
    Histogram h;
    for (std::uint64_t v : {1ULL, 2ULL, 1023ULL, 1024ULL, 5000ULL, 1'234'567ULL}) {
        INFO("v=" << v);
        CHECK(h.lowest_equivalent_value(v) <= v);
        CHECK(h.highest_equivalent_value(v) >= v);
    }
}

TEST_CASE("merge combines two histograms", "[histogram]") {
    // Per-thread histograms merged after the run, so the measurement path never
    // contends on a shared one.
    Histogram a;
    Histogram b;
    for (std::uint64_t v = 1; v <= 1000; ++v) {
        a.record(v);
    }
    for (std::uint64_t v = 1001; v <= 2000; ++v) {
        b.record(v);
    }
    CHECK(a.merge(b));
    CHECK(a.count() == 2000);
    CHECK(a.min() == 1);
    CHECK(a.max() >= 2000);
}

TEST_CASE("reset clears everything", "[histogram]") {
    Histogram h;
    for (std::uint64_t v = 1; v <= 1000; ++v) {
        h.record(v);
    }
    h.reset();
    CHECK(h.count() == 0);
    CHECK(h.percentile(99.0) == 0);
}

// ---------------------------------------------------------------------------
// Coordinated omission — the reason this class exists
// ---------------------------------------------------------------------------

TEST_CASE("coordinated omission correction backfills a stall", "[histogram][omission]") {
    // The canonical scenario. 10,000 samples at 1us apart, then one 100ms
    // stall.
    //
    // Uncorrected, that stall is a single sample: it is 1 observation in 10,001,
    // so p99.9 does not even see it and the system looks healthy.
    //
    // Corrected, the stall is what it actually was — every request that would
    // have arrived during those 100ms was delayed too, by progressively less.
    // That is ~100,000 additional degraded samples, and the tail moves by
    // orders of magnitude.
    constexpr std::uint64_t kInterval = 1'000;        // 1 us
    constexpr std::uint64_t kStall = 100'000'000;     // 100 ms

    Histogram naive;
    Histogram corrected;

    for (int i = 0; i < 10'000; ++i) {
        naive.record(kInterval);
        corrected.record_corrected(kInterval, kInterval);
    }
    naive.record(kStall);
    corrected.record_corrected(kStall, kInterval);

    // The naive histogram barely notices.
    CHECK(naive.count() == 10'001);
    CHECK(naive.percentile(99.9) < kStall);

    // The corrected one has synthesised the swallowed samples.
    CHECK(corrected.count() > 100'000);

    // And the tail tells the truth: p99.9 is now in stall territory rather than
    // sitting at the happy-path interval.
    INFO("naive p99.9    = " << naive.percentile(99.9));
    INFO("corrected p99.9 = " << corrected.percentile(99.9));
    CHECK(corrected.percentile(99.9) > naive.percentile(99.9) * 100);
}

TEST_CASE("correction is a no-op when nothing was missed", "[histogram][omission]") {
    // A sample at or under the expected interval means the generator kept up,
    // so there is nothing to backfill and the correction must not invent data.
    Histogram h;
    for (int i = 0; i < 1000; ++i) {
        h.record_corrected(500, 1000);
    }
    CHECK(h.count() == 1000);
    CHECK(h.max() <= h.highest_equivalent_value(500));
}

TEST_CASE("a zero expected interval disables correction", "[histogram][omission]") {
    // For a genuinely open-loop source — one that emits regardless of whether
    // the consumer keeps up — no sample can have been swallowed, so correcting
    // would double-count.
    Histogram h;
    h.record_corrected(1'000'000, 0);
    CHECK(h.count() == 1);
}

TEST_CASE("correction terminates on pathological input", "[histogram][omission]") {
    // The backfill loop counts down in unsigned arithmetic; an interval of 1
    // against a large value is the case that would wrap or hang if the guard
    // were wrong.
    Histogram h(1'000'000, 3);
    h.record_corrected(10'000, 1);
    CHECK(h.count() == 10'000);
    CHECK(h.min() == 1);
}

TEST_CASE("summarise reports the tail, not just the mean", "[histogram]") {
    Histogram h;
    for (std::uint64_t v = 1; v <= 100'000; ++v) {
        h.record(v);
    }
    const LatencyReport r = summarise(h);
    CHECK(r.count == 100'000);
    CHECK(r.p50 < r.p99);
    CHECK(r.p99 < r.p999);
    CHECK(r.p999 <= r.p9999);
    CHECK(r.p9999 <= r.max);
    CHECK(r.mean > 0.0);
}

// ---------------------------------------------------------------------------
// Sizing invariants
//
// The bucket-sizing loop stopped at `covered == highest_trackable`, which is
// one doubling short whenever the range is exactly a power of two. `record`
// clamps everything above the range down onto it, so on such a histogram
// EVERY oversized sample wrote one past the end of `counts_`. Caught by ASan,
// not by any test here, because every value in this file happened to be a
// non-power-of-two.
// ---------------------------------------------------------------------------

TEST_CASE("a power-of-two range does not write past the counts array",
          "[histogram][sizing]") {
    // Each of these sizes to exactly (bucket_count + 1) * sub_bucket_half_count
    // and lands its top value on the last index. Off by one and this is a heap
    // overflow, so the assertion that matters is that it runs at all.
    for (const std::uint64_t highest : {std::uint64_t{2048}, std::uint64_t{4096},
                                        std::uint64_t{65536}, std::uint64_t{1} << 20,
                                        std::uint64_t{1} << 32}) {
        for (int sig = 1; sig <= 5; ++sig) {
            Histogram h(highest, sig);
            h.record(highest);      // The clamp target: index_for's largest result.
            h.record(highest * 2);  // Clamped down onto it.
            h.record(1);
            CHECK(h.count() == 3);
            CHECK(h.max() == highest);
            CHECK(h.overflow_count() == 1);
            // The assertion that actually fails when the sizing is wrong. A
            // release-build vector write past the end is silent, so "it ran"
            // proves nothing; a sample that indexed out of bounds is missing
            // from the buckets while still counted.
            CHECK(h.bucket_total() == h.count());
            CHECK(h.percentile(100.0) >= highest);
        }
    }
}

TEST_CASE("a range beyond 2^63 is clamped rather than looped on forever",
          "[histogram][sizing]") {
    // `covered <<= 1` wraps to zero past 2^63, and `0 < highest` is true
    // forever. Construction hung. A caller passing UINT64_MAX is asking for
    // "no practical limit", so clamp and carry on rather than diverge.
    Histogram h(std::numeric_limits<std::uint64_t>::max(), 3);
    h.record(1'000'000);
    CHECK(h.count() == 1);
    CHECK(h.max() == 1'000'000);
}

TEST_CASE("merging incompatible layouts is refused, not silently wrong",
          "[histogram][sizing]") {
    // counts_.size() is (bucket_count + 1) * sub_bucket_half_count, and
    // different (range, significant_figures) pairs collide on that product
    // while mapping values to indices completely differently. Merging across
    // such a pair produced plausible, silently wrong percentiles.
    Histogram coarse(2'000'000'000'000ULL, 3);
    Histogram fine(30'000, 4);
    REQUIRE(coarse.bucket_layout() != fine.bucket_layout());

    for (int i = 0; i < 990; ++i) {
        fine.record(5'000);
    }
    for (int i = 0; i < 10; ++i) {
        fine.record(20'000);
    }
    const std::uint64_t truth = fine.percentile(50.0);

    CHECK_FALSE(coarse.merge(fine));  // Refused.
    CHECK(coarse.count() == 0);

    Histogram same(30'000, 4);
    CHECK(same.merge(fine));  // Same layout: accepted.
    CHECK(same.count() == 1'000);
    CHECK(same.percentile(50.0) == truth);
}
