// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Divergence recording.
//
// A match rate on its own is marketing. "99.7% of checksums matched" invites
// exactly one question — what was the other 0.3%? — and a verifier that cannot
// answer it has not actually verified anything.
//
// So every mismatch is captured with a cause, the sequence position, and enough
// context to reproduce it. The published match rate is then a summary of a list
// that exists, rather than a number with nothing behind it.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "crossbook/types.hpp"

namespace crossbook {

/// Why the local book disagreed with the venue.
enum class DivergenceKind : std::uint8_t {
    /// Our computed CRC32 differed from the exchange's. The book is wrong, or
    /// our understanding of the checksum input is.
    kChecksumMismatch,
    /// Continuity broke: an update was dropped, duplicated, or reordered.
    kSequenceGap,
    /// A price or quantity on the wire was not exactly representable at the
    /// instrument's configured scale. Usually means the venue changed its
    /// precision without telling anyone.
    kPrecisionLoss,
    /// A message did not match the venue's documented shape.
    kMalformedMessage,
    /// No update for longer than the staleness threshold; the feed is silent
    /// and the book can no longer be trusted to be current.
    kStaleFeed,
};

[[nodiscard]] constexpr std::string_view to_string(DivergenceKind k) noexcept {
    switch (k) {
        case DivergenceKind::kChecksumMismatch:
            return "checksum_mismatch";
        case DivergenceKind::kSequenceGap:
            return "sequence_gap";
        case DivergenceKind::kPrecisionLoss:
            return "precision_loss";
        case DivergenceKind::kMalformedMessage:
            return "malformed_message";
        case DivergenceKind::kStaleFeed:
            return "stale_feed";
    }
    return "unknown";
}

/// One recorded disagreement.
struct Divergence {
    DivergenceKind kind{};
    std::string venue;
    std::string symbol;
    Timestamp ts{0};
    SequenceId sequence{0};
    /// Exchange-supplied value, where there is one (their checksum).
    std::uint64_t expected{0};
    /// What we computed.
    std::uint64_t actual{0};
    /// Free-form context: the checksum payload, the offending token, and so on.
    std::string detail;
};

/// Collects divergences and the running totals needed to publish a match rate.
///
/// Bounded by construction. An unbounded log is a memory leak with good
/// intentions, and a verifier that dies after eight hours cannot make a claim
/// about a 24-hour capture.
class DivergenceLog {
public:
    static constexpr std::size_t kDefaultCapacity = 4096;

    explicit DivergenceLog(std::size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

    void record(Divergence d) {
        ++counts_[static_cast<std::size_t>(d.kind)];
        ++total_recorded_;
        if (entries_.size() < capacity_) {
            entries_.push_back(std::move(d));
        } else {
            ++dropped_;
        }
    }

    /// Count a successfully verified update. The denominator of the match rate.
    void record_verified() noexcept { ++verified_; }

    [[nodiscard]] const std::vector<Divergence>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::uint64_t verified() const noexcept { return verified_; }
    [[nodiscard]] std::uint64_t total_recorded() const noexcept { return total_recorded_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

    [[nodiscard]] std::uint64_t count(DivergenceKind k) const noexcept {
        return counts_[static_cast<std::size_t>(k)];
    }

    /// Verified updates as a fraction of all checked updates. Returns 1.0 when
    /// nothing has been checked yet, which callers must not mistake for
    /// evidence — `verified()` being zero is the thing to test.
    [[nodiscard]] double match_rate() const noexcept {
        const std::uint64_t total = verified_ + total_recorded_;
        return total == 0 ? 1.0 : static_cast<double>(verified_) / static_cast<double>(total);
    }

    [[nodiscard]] bool empty() const noexcept { return total_recorded_ == 0; }

    void clear() noexcept {
        entries_.clear();
        counts_ = {};
        verified_ = 0;
        total_recorded_ = 0;
        dropped_ = 0;
    }

private:
    std::size_t capacity_;
    std::vector<Divergence> entries_;
    std::array<std::uint64_t, 5> counts_{};
    std::uint64_t verified_{0};
    std::uint64_t total_recorded_{0};
    std::uint64_t dropped_{0};
};

}  // namespace crossbook
