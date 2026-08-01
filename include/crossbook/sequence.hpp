// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Sequence continuity and snapshot reconciliation.
//
// Binance publishes no checksum, so correctness there rests entirely on proving
// that no update was dropped, duplicated, or reordered between the REST
// snapshot and the live diff stream. That proof is this file.
//
// The venues do NOT agree on how to express continuity, and quietly assuming
// they do is a classic way to ship a book that is subtly wrong under load:
//
//   Binance spot     each event's U must equal the previous event's u + 1
//   Binance futures  each event carries pu, which must equal the previous u
//   Coinbase full    a single `sequence` field that must increment by exactly 1
//
// Same idea, three different wire contracts. Modelling them as one policy enum
// over one state machine keeps the difference explicit instead of letting it
// hide in per-venue copy-paste.
//
// Snapshot reconciliation for Binance, per its documented procedure:
//   - discard any event whose u is below the snapshot's lastUpdateId
//   - the first event actually applied must straddle it: U <= lastUpdateId <= u
//   - from then on, continuity applies

#pragma once

#include <cstdint>

#include "crossbook/types.hpp"

namespace crossbook {

/// Which continuity contract a venue uses.
enum class SequencePolicy : std::uint8_t {
    /// Binance spot: `U == previous u + 1`.
    kBinanceSpot,
    /// Binance USDS-M and coin-margined futures: `pu == previous u`.
    kBinanceFutures,
    /// One counter that must increment by exactly one (Coinbase `full`).
    kStrictIncrement,
};

/// Sequence identifiers carried by a single depth update.
struct UpdateIds {
    /// `U` on Binance. For kStrictIncrement, set equal to `final_id`.
    SequenceId first_id{0};
    /// `u` on Binance; `sequence` on Coinbase.
    SequenceId final_id{0};
    /// `pu` on Binance futures. Ignored by the other policies.
    SequenceId prev_id{0};
};

/// What the caller should do with an update.
enum class SequenceAction : std::uint8_t {
    /// Continuous with what came before. Apply it to the book.
    kApply,
    /// Entirely below the snapshot we already hold. Drop it silently; this is
    /// expected and normal while draining the buffer captured during the
    /// snapshot fetch.
    kDiscardStale,
    /// Continuity is broken, or no snapshot has been applied yet. The book must
    /// be cleared and re-snapshotted. Never apply an update after this.
    kResyncRequired,
};

[[nodiscard]] constexpr std::string_view to_string(SequenceAction a) noexcept {
    switch (a) {
        case SequenceAction::kApply:
            return "apply";
        case SequenceAction::kDiscardStale:
            return "discard_stale";
        case SequenceAction::kResyncRequired:
            return "resync_required";
    }
    return "unknown";
}

/// Running counts, for the health endpoint and for the README's honesty.
///
/// A feed handler that never reports its own gap rate is asking to be trusted
/// on a claim it has not measured.
struct SequenceStats {
    std::uint64_t applied{0};
    std::uint64_t discarded_stale{0};
    std::uint64_t gaps_detected{0};
    std::uint64_t snapshots_applied{0};
};

/// Tracks continuity for one instrument on one venue.
///
/// Deliberately holds no book state and performs no I/O: it is a pure state
/// machine over sequence numbers, which makes it exhaustively testable and
/// trivially fuzzable.
class SequenceTracker {
public:
    explicit SequenceTracker(SequencePolicy policy) noexcept : policy_(policy) {}

    /// Record that a fresh snapshot was applied to the book.
    /// `snapshot_id` is Binance's `lastUpdateId`, or the sequence carried by a
    /// Coinbase snapshot.
    void on_snapshot(SequenceId snapshot_id) noexcept {
        snapshot_id_ = snapshot_id;
        have_snapshot_ = true;
        straddled_ = false;
        last_final_id_ = 0;
        ++stats_.snapshots_applied;
    }

    /// Classify an incoming update. Also advances internal state, so call it
    /// exactly once per update and honour the result.
    [[nodiscard]] SequenceAction on_update(const UpdateIds& ids) noexcept {
        if (!have_snapshot_) {
            return SequenceAction::kResyncRequired;
        }

        if (policy_ == SequencePolicy::kStrictIncrement) {
            if (!straddled_) {
                // First update after a snapshot: accept it if it continues the
                // snapshot, otherwise we missed something in between.
                if (ids.final_id <= snapshot_id_) {
                    ++stats_.discarded_stale;
                    return SequenceAction::kDiscardStale;
                }
                if (ids.final_id != snapshot_id_ + 1) {
                    ++stats_.gaps_detected;
                    have_snapshot_ = false;
                    return SequenceAction::kResyncRequired;
                }
            } else if (ids.final_id != last_final_id_ + 1) {
                if (ids.final_id <= last_final_id_) {
                    ++stats_.discarded_stale;
                    return SequenceAction::kDiscardStale;  // Duplicate or replay.
                }
                ++stats_.gaps_detected;
                have_snapshot_ = false;
                return SequenceAction::kResyncRequired;
            }
            straddled_ = true;
            last_final_id_ = ids.final_id;
            ++stats_.applied;
            return SequenceAction::kApply;
        }

        // --- Binance spot and futures ---

        if (!straddled_) {
            // Drain the buffer captured while the snapshot was in flight.
            if (ids.final_id < snapshot_id_) {
                ++stats_.discarded_stale;
                return SequenceAction::kDiscardStale;
            }
            // The first applied event must straddle the snapshot id.
            if (!(ids.first_id <= snapshot_id_ && ids.final_id >= snapshot_id_)) {
                // The stream has already moved past our snapshot: it is stale.
                ++stats_.gaps_detected;
                have_snapshot_ = false;
                return SequenceAction::kResyncRequired;
            }
            straddled_ = true;
            last_final_id_ = ids.final_id;
            ++stats_.applied;
            return SequenceAction::kApply;
        }

        const bool continuous = (policy_ == SequencePolicy::kBinanceSpot)
                                    ? (ids.first_id == last_final_id_ + 1)
                                    : (ids.prev_id == last_final_id_);
        if (!continuous) {
            if (ids.final_id <= last_final_id_) {
                ++stats_.discarded_stale;
                return SequenceAction::kDiscardStale;  // Duplicate delivery.
            }
            ++stats_.gaps_detected;
            have_snapshot_ = false;
            return SequenceAction::kResyncRequired;
        }

        last_final_id_ = ids.final_id;
        ++stats_.applied;
        return SequenceAction::kApply;
    }

    /// True once a snapshot has been applied and no gap has been seen since.
    [[nodiscard]] bool synced() const noexcept { return have_snapshot_; }

    /// True once an update has actually been applied on top of the snapshot.
    [[nodiscard]] bool straddled() const noexcept { return straddled_; }

    [[nodiscard]] SequenceId last_applied_id() const noexcept { return last_final_id_; }
    [[nodiscard]] const SequenceStats& stats() const noexcept { return stats_; }
    [[nodiscard]] SequencePolicy policy() const noexcept { return policy_; }

    /// Force a resync, e.g. after a socket reconnect or a staleness timeout.
    void invalidate() noexcept {
        have_snapshot_ = false;
        straddled_ = false;
    }

private:
    SequencePolicy policy_;
    SequenceId snapshot_id_{0};
    SequenceId last_final_id_{0};
    bool have_snapshot_{false};
    bool straddled_{false};
    SequenceStats stats_{};
};

}  // namespace crossbook
