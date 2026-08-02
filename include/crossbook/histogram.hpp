// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// A high dynamic range histogram, and the coordinated-omission correction that
// makes its tail percentiles mean anything.
//
// WHY NOT JUST STORE A MEAN:
//
// Latency distributions are heavily right-tailed. A mean hides precisely the
// tail you would be paid to fix, so reporting one is worse than reporting
// nothing — it looks like a measurement and isn't. Everything here is designed
// to answer "what is p99.99" over a run of hundreds of millions of samples
// without storing them.
//
// Log-linear bucketing, following Gil Tene's HdrHistogram: values are grouped
// into power-of-two buckets, each linearly subdivided, so relative precision is
// constant across the whole range. Three significant figures over [1ns, 1hr]
// costs about 30 KB and every record() is a handful of integer ops with no
// allocation and no branching on value magnitude.
//
// WHY COORDINATED OMISSION HAS ITS OWN FUNCTION:
//
// If a measurement loop stalls, it stops issuing requests during the stall. The
// one slow sample gets recorded once, when in reality every request that would
// have arrived during the stall was also delayed. The tail comes out orders of
// magnitude too optimistic, and the result still looks like a careful
// measurement.
//
// `record_corrected` takes the interval at which samples were *supposed* to
// arrive and backfills the samples the stall swallowed. It is the difference
// between a p99.9 that describes reality and one that describes your harness.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace crossbook {

/// Fixed-precision histogram over a wide value range.
///
/// Values are unitless; the replay harness records nanoseconds. Recording is
/// allocation-free after construction, which is what allows it to sit in the
/// measurement path without perturbing the thing it measures.
class Histogram {
public:
    /// `highest_trackable` bounds the range; values above it are clamped and
    /// counted (see `overflow_count`). `significant_figures` fixes relative
    /// precision — 3 means values within 0.1% of each other share a bucket.
    explicit Histogram(std::uint64_t highest_trackable = 3'600'000'000'000ULL,
                       int significant_figures = 3) {
        if (significant_figures < 1) {
            significant_figures = 1;
        }
        if (significant_figures > 5) {
            significant_figures = 5;
        }
        if (highest_trackable < 2) {
            highest_trackable = 2;
        }
        // Past 2^63 the `covered <<= 1` below wraps to zero and the sizing loop
        // never terminates. A caller passing something near UINT64_MAX means
        // "no practical ceiling", so give them the largest one that can be
        // expressed rather than hanging at construction.
        if (highest_trackable > (1ULL << 62)) {
            highest_trackable = 1ULL << 62;
        }
        highest_trackable_ = highest_trackable;

        // Smallest power of two that still gives unit resolution across the
        // largest value we want single-unit precision for.
        std::uint64_t largest_single_unit = 2;
        for (int i = 0; i < significant_figures; ++i) {
            largest_single_unit *= 10;
        }
        sub_bucket_magnitude_ = 1;
        while ((1ULL << sub_bucket_magnitude_) < largest_single_unit) {
            ++sub_bucket_magnitude_;
        }
        sub_bucket_half_magnitude_ = sub_bucket_magnitude_ - 1;
        sub_bucket_count_ = 1ULL << sub_bucket_magnitude_;
        sub_bucket_half_count_ = sub_bucket_count_ >> 1;
        sub_bucket_mask_ = sub_bucket_count_ - 1;

        // Enough power-of-two buckets to reach the top of the range.
        //
        // The comparison is `<=`, not `<`. Stopping at `covered ==
        // highest_trackable_` leaves the top value indexing one past the end:
        // `record` clamps everything above the range down onto
        // `highest_trackable_`, so on a power-of-two range every oversized
        // sample wrote out of bounds. Only exact powers of two are affected,
        // which is why the default of 3'600'000'000'000 never showed it.
        bucket_count_ = 1;
        std::uint64_t covered = sub_bucket_count_;
        while (covered <= highest_trackable_) {
            covered <<= 1;
            ++bucket_count_;
        }

        counts_.assign(static_cast<std::size_t>((bucket_count_ + 1) * sub_bucket_half_count_), 0);
    }

    /// Record one observation.
    void record(std::uint64_t value) noexcept {
        if (value > highest_trackable_) {
            ++overflow_count_;
            value = highest_trackable_;
        }
        ++counts_[index_for(value)];
        ++total_count_;
        total_sum_ += value;
        if (value > max_ || total_count_ == 1) {
            max_ = value;
        }
        if (value < min_ || total_count_ == 1) {
            min_ = value;
        }
    }

    /// Record one observation, then backfill the samples a stall swallowed.
    ///
    /// THIS IS THE ONE THAT MATTERS FOR LATENCY.
    ///
    /// If `value` exceeds `expected_interval`, the system was stalled long
    /// enough that additional samples should have been taken and weren't.
    /// Synthesise them at decreasing latencies, exactly as they would have been
    /// observed had the load generator not been blocked.
    ///
    /// Passing `expected_interval == 0` disables correction, which is correct
    /// for a genuinely open-loop source where no sample could have been missed.
    void record_corrected(std::uint64_t value, std::uint64_t expected_interval) noexcept {
        record(value);
        if (expected_interval == 0 || value <= expected_interval) {
            return;
        }
        for (std::uint64_t missing = value - expected_interval; missing >= expected_interval;
             missing -= expected_interval) {
            record(missing);
            if (missing < expected_interval) {
                break;  // Guard against unsigned wraparound.
            }
        }
    }

    /// Value at a percentile in [0, 100]. Returns 0 for an empty histogram.
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (total_count_ == 0) {
            return 0;
        }
        p = (std::clamp)(p, 0.0, 100.0);

        // Round up: the reported value must be one an observation actually
        // reached, never an interpolation between buckets.
        auto target = static_cast<std::uint64_t>(
            (p / 100.0) * static_cast<double>(total_count_) + 0.5);
        if (target == 0) {
            target = 1;
        }
        if (target > total_count_) {
            target = total_count_;
        }

        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= target) {
                // Report the bucket's upper bound, so the answer is never
                // optimistic — but never above the largest value actually
                // observed. Both are valid upper bounds on the percentile;
                // clamping just picks the tighter one, and it keeps
                // percentile(100) == max() instead of returning a figure no
                // observation ever reached. A latency table where p99.99
                // exceeds the maximum is one nobody trusts twice.
                return (std::min)(highest_equivalent_value(value_at_index(i)), max_);
            }
        }
        return max_;
    }

    // The parentheses around the names are not decoration. <windows.h> defines
    // function-like `min` and `max` macros unless the consumer sets NOMINMAX,
    // and a declarator spelled `min()` is a zero-argument invocation of a
    // two-argument macro: the error lands inside this file, on a desk that did
    // nothing wrong. `(min)()` is not a macro invocation, so it survives.
    // Requiring NOMINMAX of a consumer is the workaround, not the fix.
    [[nodiscard]] std::uint64_t(min)() const noexcept { return total_count_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t(max)() const noexcept { return total_count_ == 0 ? 0 : max_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return total_count_; }
    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_count_; }

    /// Reported alongside percentiles, never instead of them.
    [[nodiscard]] double mean() const noexcept {
        return total_count_ == 0 ? 0.0
                                 : static_cast<double>(total_sum_) /
                                       static_cast<double>(total_count_);
    }

    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0ULL);
        total_count_ = 0;
        total_sum_ = 0;
        overflow_count_ = 0;
        min_ = (std::numeric_limits<std::uint64_t>::max)();
        max_ = 0;
    }

    /// Fold another histogram in. Used to combine per-thread histograms without
    /// contending on a shared one in the measurement path.
    /// Returns false, and changes nothing, if the layouts differ.
    ///
    /// Comparing `counts_.size()` was not enough: that size is
    /// `(bucket_count + 1) * sub_bucket_half_count`, and distinct
    /// (range, significant_figures) pairs collide on the product while mapping
    /// values to indices completely differently — (2e12, 3) and (30'000, 4)
    /// both size to 32768. Merging across such a pair added counts bucket-by-
    /// bucket into slots that meant something else and reported percentiles
    /// that were off by 3x while looking entirely plausible. Silently wrong
    /// tail numbers are worse than no tail numbers, so this reports.
    [[nodiscard]] bool merge(const Histogram& other) {
        if (other.bucket_layout() != bucket_layout()) {
            return false;
        }
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            counts_[i] += other.counts_[i];
        }
        if (other.total_count_ > 0) {
            min_ = (total_count_ == 0) ? other.min_ : (std::min)(min_, other.min_);
            max_ = (std::max)(max_, other.max_);
        }
        total_count_ += other.total_count_;
        total_sum_ += other.total_sum_;
        overflow_count_ += other.overflow_count_;
        return true;
    }

    /// Total samples actually resident in buckets. Must equal `count()`.
    ///
    /// These can only diverge if some value indexed outside `counts_`, which
    /// is a sizing bug rather than a recording one — and a silent one, because
    /// the write lands in whatever follows the allocation and the sample
    /// simply vanishes from every percentile. Asserting the identity is the
    /// only cheap way to catch that without a sanitizer, so tests check it
    /// rather than checking that recording "ran".
    [[nodiscard]] std::uint64_t bucket_total() const noexcept {
        std::uint64_t sum = 0;
        for (const std::uint64_t c : counts_) {
            sum += c;
        }
        return sum;
    }

    /// The value→index mapping, as a comparable triple.
    ///
    /// Two histograms may be merged only if these agree. Exposed because
    /// "are these two comparable" is a question callers legitimately have
    /// before they have samples to lose.
    [[nodiscard]] std::array<std::uint64_t, 3> bucket_layout() const noexcept {
        return {sub_bucket_magnitude_, bucket_count_, highest_trackable_};
    }

    /// The smallest value that would land in the same bucket as `value`.
    [[nodiscard]] std::uint64_t lowest_equivalent_value(std::uint64_t value) const noexcept {
        const std::uint64_t bucket = bucket_index(value);
        const std::uint64_t sub = sub_bucket_index(value, bucket);
        return sub << bucket;
    }

    /// The largest value that would land in the same bucket as `value`.
    [[nodiscard]] std::uint64_t highest_equivalent_value(std::uint64_t value) const noexcept {
        return lowest_equivalent_value(value) + bucket_width(value) - 1;
    }

private:
    [[nodiscard]] std::uint64_t bucket_width(std::uint64_t value) const noexcept {
        return 1ULL << bucket_index(value);
    }

    [[nodiscard]] std::uint64_t bucket_index(std::uint64_t value) const noexcept {
        // ORing in the mask guarantees at least sub_bucket_magnitude_ bits are
        // set, which keeps the result non-negative for small values without a
        // branch.
        const std::uint64_t masked = value | sub_bucket_mask_;
        const int leading = count_leading_zeros(masked);
        const int pow2ceiling = 64 - leading;
        return static_cast<std::uint64_t>(pow2ceiling) - sub_bucket_magnitude_;
    }

    [[nodiscard]] std::uint64_t sub_bucket_index(std::uint64_t value,
                                                 std::uint64_t bucket) const noexcept {
        return value >> bucket;
    }

    [[nodiscard]] std::size_t index_for(std::uint64_t value) const noexcept {
        const std::uint64_t bucket = bucket_index(value);
        const std::uint64_t sub = sub_bucket_index(value, bucket);
        const std::uint64_t base = (bucket + 1) << sub_bucket_half_magnitude_;
        const std::uint64_t offset = sub - sub_bucket_half_count_;
        return static_cast<std::size_t>(base + offset);
    }

    [[nodiscard]] std::uint64_t value_at_index(std::size_t index) const noexcept {
        auto bucket = static_cast<std::int64_t>(index >> sub_bucket_half_magnitude_) - 1;
        auto sub = static_cast<std::int64_t>(index & (sub_bucket_half_count_ - 1)) +
                   static_cast<std::int64_t>(sub_bucket_half_count_);
        if (bucket < 0) {
            sub -= static_cast<std::int64_t>(sub_bucket_half_count_);
            bucket = 0;
        }
        return static_cast<std::uint64_t>(sub) << static_cast<std::uint64_t>(bucket);
    }

    /// Count-leading-zeros.
    ///
    /// Was a shift-and-count loop — up to 64 iterations, and it ran once per
    /// `record()`, which is inside the replay measurement loop. Measured at
    /// ~15 ns of a 22.5 ns `record()`; `std::countl_zero` is ~1.7 ns and
    /// compiles to a single instruction. The header above claims every record
    /// is "a handful of integer ops", and with this it finally is.
    ///
    /// Also removes a latent hang: the old loop never terminated on zero. The
    /// caller ORs in a non-zero mask so it was unreachable, but "unreachable"
    /// was a property of the caller, not of this function.
    [[nodiscard]] static int count_leading_zeros(std::uint64_t value) noexcept {
        return std::countl_zero(value);
    }

    std::uint64_t highest_trackable_{0};
    std::uint64_t sub_bucket_magnitude_{0};
    std::uint64_t sub_bucket_half_magnitude_{0};
    std::uint64_t sub_bucket_count_{0};
    std::uint64_t sub_bucket_half_count_{0};
    std::uint64_t sub_bucket_mask_{0};
    std::uint64_t bucket_count_{0};

    std::vector<std::uint64_t> counts_;
    std::uint64_t total_count_{0};
    std::uint64_t total_sum_{0};
    std::uint64_t overflow_count_{0};
    std::uint64_t min_{(std::numeric_limits<std::uint64_t>::max)()};
    std::uint64_t max_{0};
};

/// The percentiles worth publishing. A latency report that stops at p99 is
/// hiding the part that matters.
struct LatencyReport {
    std::uint64_t count{0};
    std::uint64_t min{0};
    std::uint64_t p50{0};
    std::uint64_t p90{0};
    std::uint64_t p99{0};
    std::uint64_t p999{0};
    std::uint64_t p9999{0};
    std::uint64_t max{0};
    double mean{0.0};
};

[[nodiscard]] inline LatencyReport summarise(const Histogram& h) {
    // `(h.min)()` for the same reason the declarations above are parenthesised:
    // a call spelled `h.min()` is still a macro invocation to the preprocessor.
    return LatencyReport{h.count(),           (h.min)(),          h.percentile(50.0),
                         h.percentile(90.0),  h.percentile(99.0), h.percentile(99.9),
                         h.percentile(99.99), (h.max)(),          h.mean()};
}

}  // namespace crossbook
