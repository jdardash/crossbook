// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The feed handler: decode, check continuity, apply, verify, recover.
//
// This is where the pieces become a system, and where the interesting design
// question lives — not "did we detect a problem" but "what do we do next".
//
// THE RULE THIS CLASS EXISTS TO ENFORCE:
//
//   A book that is known to be wrong must never be served, and must never be
//   updated further, until it has been rebuilt from a snapshot.
//
// The failure that actually costs money is not a dropped message. It is
// noticing a dropped message and continuing anyway, because the resulting book
// looks entirely plausible and disagrees with the venue in ways no downstream
// consumer can detect. Every path here that discovers a problem ends in the
// same place: stop, mark unsynced, demand a snapshot.
//
// Verification is per-venue because the venues differ:
//   - Kraken publishes a CRC32 per message, so the book is checked against the
//     exchange on every update and a mismatch triggers recovery.
//   - Binance publishes no checksum, so correctness rests on sequence
//     continuity and REST snapshot reconciliation alone.
// Both are handled by the same state machine; which evidence is available is a
// property of the message, not of the code path.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"
#include "crossbook/divergence.hpp"
#include "crossbook/sequence.hpp"
#include "crossbook/venue.hpp"

namespace crossbook {

/// What happened to a frame, and what the caller must do about it.
enum class FeedStatus : std::uint8_t {
    /// Applied to the book, and verified if the venue supplied evidence.
    kApplied,
    /// Not a book message, or a stale duplicate. Nothing to do.
    kIgnored,
    /// The book cannot be trusted. Resubscribe or refetch a snapshot, then feed
    /// it in via `apply_snapshot`. Do not read the book until then.
    kNeedsSnapshot,
    /// The frame could not be decoded. Logged; the book is untouched.
    kRejected,
};

[[nodiscard]] constexpr std::string_view to_string(FeedStatus s) noexcept {
    switch (s) {
        case FeedStatus::kApplied:
            return "applied";
        case FeedStatus::kIgnored:
            return "ignored";
        case FeedStatus::kNeedsSnapshot:
            return "needs_snapshot";
        case FeedStatus::kRejected:
            return "rejected";
    }
    return "unknown";
}

/// Running totals for the health endpoint and the published match rate.
struct FeedStats {
    std::uint64_t frames{0};
    std::uint64_t applied{0};
    std::uint64_t ignored{0};
    std::uint64_t rejected{0};
    std::uint64_t resyncs_requested{0};
    std::uint64_t snapshots_applied{0};
    std::uint64_t checksums_verified{0};
    std::uint64_t checksum_mismatches{0};
    /// Levels dropped for falling outside a depth-limited subscription. A
    /// steady trickle is normal and expected; zero on a depth-limited feed
    /// means the depth was never configured, which is worth being able to see.
    std::uint64_t levels_trimmed{0};
};

/// Drives one instrument on one venue.
///
/// `Decoder` supplies `decode(std::string_view) -> const DecodedMessage&`.
/// `BookT` is any BasicL2Book instantiation.
template <typename Decoder, typename BookT = ArrayBook>
class Feed {
public:
    /// `depth` is the number of levels the subscription covers, or 0 for a full
    /// book. It is not cosmetic: a depth-limited venue never tells you that a
    /// level fell out of the window, so a feed that does not trim accumulates
    /// levels the venue stopped tracking and eventually fails its checksums.
    /// See `BasicL2Book::trim` for the measurement.
    Feed(std::string venue, Decoder decoder, SequencePolicy policy, std::size_t depth = 0)
        : venue_(std::move(venue)),
          decoder_(std::move(decoder)),
          book_(decoder_.spec()),
          tracker_(policy),
          depth_(depth) {}

    [[nodiscard]] const BookT& book() const noexcept { return book_; }
    [[nodiscard]] const DivergenceLog& divergences() const noexcept { return log_; }
    [[nodiscard]] const SequenceTracker& sequence() const noexcept { return tracker_; }
    [[nodiscard]] const FeedStats& stats() const noexcept { return stats_; }
    /// Levels per side this subscription covers; 0 for a full book.
    [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
    [[nodiscard]] Decoder& decoder() noexcept { return decoder_; }

    /// True when the book reflects the venue and may be read.
    ///
    /// Callers must consult this rather than assuming. A book that has lost
    /// sync still holds plausible-looking levels; that is exactly what makes it
    /// dangerous.
    [[nodiscard]] bool synced() const noexcept { return synced_; }

    /// The proportion of checked updates that matched the exchange.
    [[nodiscard]] double match_rate() const noexcept { return log_.match_rate(); }

    /// Feed one raw websocket frame.
    [[nodiscard]] FeedStatus handle(std::string_view frame) {
        ++stats_.frames;
        const DecodedMessage& msg = decoder_.decode(frame);

        if (!msg.ok()) {
            ++stats_.rejected;
            log_.record(Divergence{msg.error == DecodeError::kPrecisionLoss
                                       ? DivergenceKind::kPrecisionLoss
                                       : DivergenceKind::kMalformedMessage,
                                   venue_, std::string(msg.symbol), msg.ts, msg.ids.final_id, 0, 0,
                                   std::string(msg.bad_token.empty() ? to_string(msg.error)
                                                                     : msg.bad_token)});
            // A malformed frame says nothing about whether the book is still
            // correct, so sync is left alone. A precision change does, and the
            // divergence log is what surfaces it.
            return FeedStatus::kRejected;
        }

        switch (msg.kind) {
            case MessageKind::kIgnored:
            case MessageKind::kVenueError:
                ++stats_.ignored;
                return FeedStatus::kIgnored;
            case MessageKind::kSnapshot:
                return apply_snapshot_message(msg);
            case MessageKind::kUpdate:
                return apply_update_message(msg);
        }
        ++stats_.ignored;
        return FeedStatus::kIgnored;
    }

    /// Apply an out-of-band snapshot, e.g. a Binance REST depth response
    /// decoded via `decode_snapshot`. Clears the book first.
    [[nodiscard]] FeedStatus apply_snapshot(const DecodedMessage& msg) {
        return apply_snapshot_message(msg);
    }

    /// Force a resync — used on socket reconnect, or when a staleness timeout
    /// fires and the sequence numbers look fine but the connection did not.
    void invalidate(DivergenceKind reason = DivergenceKind::kStaleFeed) {
        if (synced_) {
            log_.record(Divergence{reason, venue_, book_.spec().symbol, last_ts_,
                                   tracker_.last_applied_id(), 0, 0, "feed invalidated"});
        }
        synced_ = false;
        tracker_.invalidate();
        ++stats_.resyncs_requested;
    }

    /// Declare the feed stale if nothing has arrived within `max_gap_ns`.
    ///
    /// A silent feed is indistinguishable from a quiet market by looking at the
    /// book alone, and serving a ten-minute-old book as current is its own kind
    /// of wrong.
    [[nodiscard]] bool check_staleness(Timestamp now, Timestamp max_gap_ns) {
        if (!synced_ || last_ts_ == 0 || max_gap_ns <= 0) {
            return false;
        }
        if (now - last_ts_ > max_gap_ns) {
            invalidate(DivergenceKind::kStaleFeed);
            return true;
        }
        return false;
    }

private:
    [[nodiscard]] FeedStatus apply_snapshot_message(const DecodedMessage& msg) {
        // A failed decode is not an empty book. Without this, a truncated REST
        // response cleared the book, applied zero levels, and set synced_ =
        // true — an empty book that reports itself as current, which is the
        // exact failure the rest of this file exists to prevent. The caller
        // gets kRejected and must fetch the snapshot again.
        if (!msg.ok()) {
            ++stats_.rejected;
            log_.record(Divergence{DivergenceKind::kMalformedMessage, venue_,
                                   std::string(book_.spec().symbol), msg.ts, msg.ids.final_id, 0, 0,
                                   std::string(msg.bad_token.empty() ? to_string(msg.error)
                                                                     : msg.bad_token)});
            return FeedStatus::kRejected;
        }

        // A snapshot is a replacement, never a merge. Applying one onto stale
        // state leaves phantom levels that no later update will ever clear,
        // because the venue has no reason to send a delete for a level it does
        // not know you invented.
        book_.clear();
        for (const LevelUpdate& lvl : msg.levels) {
            book_.apply(lvl.side, lvl.price, lvl.qty);
        }
        stats_.levels_trimmed += book_.trim(depth_);
        book_.set_last_update(msg.ts);
        last_ts_ = msg.ts;

        if (msg.has_ids) {
            tracker_.on_snapshot(msg.ids.final_id);
        }
        synced_ = true;
        ++stats_.snapshots_applied;
        ++stats_.applied;

        // A snapshot carrying a checksum is verifiable immediately, and a
        // mismatch here means the decoder disagrees with the venue about the
        // message itself — worth knowing before any update lands on top.
        if (msg.has_checksum && !verify_checksum(msg)) {
            return request_resync();
        }
        return FeedStatus::kApplied;
    }

    [[nodiscard]] FeedStatus apply_update_message(const DecodedMessage& msg) {
        if (!synced_) {
            return FeedStatus::kNeedsSnapshot;
        }

        // Venues that number their updates get continuity checked first: there
        // is no point verifying a book we already know is missing an update.
        if (msg.has_ids) {
            switch (tracker_.on_update(msg.ids)) {
                case SequenceAction::kDiscardStale:
                    ++stats_.ignored;
                    return FeedStatus::kIgnored;
                case SequenceAction::kResyncRequired:
                    log_.record(Divergence{DivergenceKind::kSequenceGap, venue_,
                                           std::string(msg.symbol), msg.ts, msg.ids.final_id,
                                           tracker_.last_applied_id(), msg.ids.first_id,
                                           "continuity broken; book must be rebuilt"});
                    return request_resync();
                case SequenceAction::kApply:
                    break;
            }
        }

        for (const LevelUpdate& lvl : msg.levels) {
            book_.apply(lvl.side, lvl.price, lvl.qty);
        }
        // Trim BEFORE verifying. The checksum covers the top ten levels, so a
        // stale level that has re-entered the top ten is exactly what the
        // checksum is about to catch - and exactly what trimming prevents.
        stats_.levels_trimmed += book_.trim(depth_);
        book_.set_last_update(msg.ts);
        last_ts_ = msg.ts;
        ++stats_.applied;

        if (msg.has_checksum && !verify_checksum(msg)) {
            return request_resync();
        }
        return FeedStatus::kApplied;
    }

    /// Recompute the exchange's checksum over local state and compare.
    [[nodiscard]] bool verify_checksum(const DecodedMessage& msg) {
        ++stats_.checksums_verified;
        const std::uint32_t local = kraken_checksum(book_);
        if (local == msg.checksum) {
            log_.record_verified();
            return true;
        }

        ++stats_.checksum_mismatches;
        // The payload is captured here, allocating, precisely because this path
        // is rare. Reporting "expected X, got Y" without the input that
        // produced Y makes a mismatch effectively undebuggable.
        log_.record(Divergence{DivergenceKind::kChecksumMismatch, venue_, std::string(msg.symbol),
                               msg.ts, msg.ids.final_id, msg.checksum, local,
                               kraken_checksum_payload(book_)});
        return false;
    }

    [[nodiscard]] FeedStatus request_resync() {
        synced_ = false;
        tracker_.invalidate();
        ++stats_.resyncs_requested;
        return FeedStatus::kNeedsSnapshot;
    }

    std::string venue_;
    Decoder decoder_;
    BookT book_;
    SequenceTracker tracker_;
    std::size_t depth_{0};
    DivergenceLog log_;
    FeedStats stats_{};
    Timestamp last_ts_{0};
    bool synced_{false};
};

}  // namespace crossbook
