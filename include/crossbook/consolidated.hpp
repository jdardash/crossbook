// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// A consolidated cross-venue book.
//
// THERE IS NO NBBO IN CRYPTO.
//
// Equities have Reg NMS, a SIP, a consolidated tape, and a legally defined
// national best bid and offer. Crypto has none of that. There is no authority
// that says what the best price is, no shared clock, and no obligation on any
// venue to agree with any other. Building the equivalent is therefore not a
// lookup, it is a set of judgement calls, and the honest thing is to make each
// one explicit rather than bury it:
//
//   1. CLOCK SKEW. Venue timestamps are not comparable. Two venues can disagree
//      by seconds, and neither is wrong from its own point of view. This file
//      uses local receive time for staleness and never compares venue clocks to
//      each other.
//
//   2. STALENESS. A venue that has gone quiet still has a book in memory, and
//      it looks exactly like a venue with a genuinely stable market. Quoting a
//      stale venue as best is worse than excluding it, so entries past a
//      configured age are dropped from consideration entirely.
//
//   3. FEES. This is the one that surprises people. Taker fees differ by venue
//      and by tier, and they are large relative to crypto spreads. The venue
//      with the best headline price is frequently not the cheapest to trade,
//      and the ranking can invert at a different fee tier for the same book.
//      Comparing raw prices across venues is simply wrong.
//
//   4. SIZE. The ranking changes again with size. A venue can show the best
//      price on one coin and the worst on fifty, because the touch is thin.
//      This is why `best_execution` takes a quantity rather than returning a
//      quote: a size-free "best venue" is not a well-defined question.
//
// What this class does NOT do is route, split, or place orders. It reports what
// the market looks like. Deciding what to do about that is a strategy concern
// and lives outside this library — see the note on scope in the README.

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/execution.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

/// Taker fees for one venue, in basis points of notional.
///
/// Only taker fees appear here because this models crossing the spread. Maker
/// rebates would change the sign and belong to a different question.
struct FeeSchedule {
    /// Hundredths of a basis point. Use from_bps(26) for a 26 bps taker fee.
    CentiBps taker{0};
};

/// One venue's contribution to the consolidated view.
struct VenueQuote {
    std::string venue;
    Level bid{};
    Level ask{};
    bool has_bid{false};
    bool has_ask{false};

    /// LOCAL receive time, not the venue's own timestamp. Venue clocks are not
    /// comparable to each other; local time is.
    Timestamp received_at{0};

    FeeSchedule fees{};

    /// False when the venue has lost sync. A book that is known wrong must not
    /// contribute to a best-price calculation.
    bool synced{true};
};

/// A venue's price adjusted for what it actually costs to trade there.
struct EffectivePrice {
    std::string venue;
    Price quoted{};
    /// Quoted price adjusted by the taker fee, in the direction that costs
    /// money: buys pay more, sells receive less.
    Price effective{};
    Qty size{};

    friend bool operator==(const EffectivePrice&, const EffectivePrice&) = default;
};

/// The outcome of asking which venue to trade a given size on.
struct BestExecution {
    std::string venue;
    Execution execution{};
    /// VWAP after the venue's taker fee.
    Price effective_vwap{};
    /// How many venues were considered, after staleness and sync filtering.
    std::size_t venues_considered{0};
    bool found{false};
};

/// Consolidated view across venues for one instrument.
///
/// Deliberately holds copies of each venue's touch rather than references to
/// live books: the consolidated view is a snapshot taken at a moment, and
/// holding pointers into books being mutated by other threads would make every
/// answer racy.
class ConsolidatedBook {
public:
    /// Entries older than this are excluded. Defaults to five seconds, which is
    /// generous for crypto and deliberately so — the failure mode being avoided
    /// is quoting a dead feed, not being slightly conservative.
    static constexpr Timestamp kDefaultMaxAgeNs = 5'000'000'000;

    explicit ConsolidatedBook(InstrumentSpec spec, Timestamp max_age_ns = kDefaultMaxAgeNs)
        : spec_(std::move(spec)), max_age_ns_(max_age_ns) {}

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] Timestamp max_age_ns() const noexcept { return max_age_ns_; }

    /// Insert or replace a venue's quote.
    void update(VenueQuote quote) {
        for (VenueQuote& existing : quotes_) {
            if (existing.venue == quote.venue) {
                existing = std::move(quote);
                return;
            }
        }
        quotes_.push_back(std::move(quote));
    }

    /// Drop a venue entirely, e.g. on permanent disconnect.
    void remove(std::string_view venue) {
        for (auto it = quotes_.begin(); it != quotes_.end(); ++it) {
            if (it->venue == venue) {
                quotes_.erase(it);
                return;
            }
        }
    }

    [[nodiscard]] std::size_t venue_count() const noexcept { return quotes_.size(); }
    [[nodiscard]] const std::vector<VenueQuote>& quotes() const noexcept { return quotes_; }

    /// Is this venue currently usable as of `now`?
    ///
    /// Requires that it is synced, has a quote on the side in question, and has
    /// been heard from recently.
    [[nodiscard]] bool usable(const VenueQuote& q, Side side, Timestamp now) const noexcept {
        if (!q.synced) {
            return false;
        }
        if (side == Side::kBid ? !q.has_bid : !q.has_ask) {
            return false;
        }
        if (max_age_ns_ > 0 && q.received_at > 0 && now > q.received_at &&
            (now - q.received_at) > max_age_ns_) {
            return false;
        }
        return true;
    }

    /// Every usable venue's price on `side`, adjusted for taker fees, best
    /// first.
    ///
    /// "Best" means best *effective* price. That ordering routinely differs
    /// from the raw-price ordering, which is the whole reason this returns
    /// effective prices rather than quotes.
    [[nodiscard]] std::vector<EffectivePrice> ranked(Side side, Timestamp now) const {
        std::vector<EffectivePrice> out;
        out.reserve(quotes_.size());

        for (const VenueQuote& q : quotes_) {
            if (!usable(q, side, now)) {
                continue;
            }
            const Level& level = (side == Side::kBid) ? q.bid : q.ask;
            out.push_back(EffectivePrice{q.venue, level.price,
                                         Price{apply_fee(level.price.ticks, side, q.fees)},
                                         level.qty});
        }

        // Taking the ask means buying: cheaper effective price is better.
        // Taking the bid means selling: higher effective price is better.
        const bool ascending = (side == Side::kAsk);
        for (std::size_t i = 1; i < out.size(); ++i) {
            EffectivePrice key = out[i];
            std::size_t j = i;
            while (j > 0 && (ascending ? (out[j - 1].effective.ticks > key.effective.ticks)
                                       : (out[j - 1].effective.ticks < key.effective.ticks))) {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = key;
        }
        return out;
    }

    /// The best effective price on `side`, if any venue is usable.
    [[nodiscard]] bool best(Side side, Timestamp now, EffectivePrice& out) const {
        const std::vector<EffectivePrice> order = ranked(side, now);
        if (order.empty()) {
            return false;
        }
        out = order.front();
        return true;
    }

    /// Consolidated spread in ticks, fee-inclusive. Exact: no division.
    ///
    /// Prefer this when comparing the same instrument over time. `spread` is
    /// for comparing across instruments, where ticks are not commensurable.
    [[nodiscard]] bool spread_ticks(Timestamp now, std::int64_t& out) const {
        EffectivePrice best_bid{};
        EffectivePrice best_ask{};
        if (!best(Side::kBid, now, best_bid) || !best(Side::kAsk, now, best_ask)) {
            return false;
        }
        out = best_ask.effective.ticks - best_bid.effective.ticks;
        return true;
    }

    /// Consolidated spread in hundredths of a basis point, fee-inclusive.
    ///
    /// Can legitimately be negative: two venues genuinely can cross, and hiding
    /// that behind a clamp would erase the only interesting reading this number
    /// ever produces. It is not necessarily an arbitrage — fees, latency, and
    /// withdrawal constraints all live between the quote and the trade — but it
    /// is real and worth surfacing.
    [[nodiscard]] bool spread(Timestamp now, CentiBps& out) const {
        EffectivePrice best_bid{};
        EffectivePrice best_ask{};
        if (!best(Side::kBid, now, best_bid) || !best(Side::kAsk, now, best_ask)) {
            return false;
        }
        const std::int64_t mid = (best_bid.effective.ticks + best_ask.effective.ticks) / 2;
        if (mid == 0) {
            return false;
        }
        const std::int64_t delta = best_ask.effective.ticks - best_bid.effective.ticks;
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        const std::int64_t magnitude = delta < 0 ? -delta : delta;
        if (magnitude != 0 && magnitude > kMax / 1'000'000) {
            return false;
        }
        out = (delta * 1'000'000) / mid;
        return true;
    }

    /// True when the best bid on one venue is at or above the best ask on
    /// another, after fees.
    ///
    /// Compares prices directly rather than deriving this from `spread`.
    /// Crossing is a price relation, and routing it through a ratio makes it a
    /// hostage to rounding: an earlier version asked whether the spread in
    /// whole basis points was <= 0, which reported every sub-bps spread — that
    /// is to say, most healthy crypto books — as crossed.
    [[nodiscard]] bool crossed(Timestamp now) const {
        EffectivePrice best_bid{};
        EffectivePrice best_ask{};
        if (!best(Side::kBid, now, best_bid) || !best(Side::kAsk, now, best_ask)) {
            return false;
        }
        return best_bid.effective.ticks >= best_ask.effective.ticks;
    }

private:
    /// Adjust a quoted price by a taker fee, in the direction that costs money.
    [[nodiscard]] static std::int64_t apply_fee(std::int64_t price, Side side,
                                                const FeeSchedule& fees) noexcept {
        const std::int64_t adjustment = (price * fees.taker) / 1'000'000;
        // Lifting an ask costs the fee on top; hitting a bid nets it out.
        return side == Side::kAsk ? price + adjustment : price - adjustment;
    }

    InstrumentSpec spec_;
    Timestamp max_age_ns_;
    std::vector<VenueQuote> quotes_;
};

/// Pick the venue that fills `wanted` most cheaply, walking each venue's real
/// depth rather than comparing touches.
///
/// `books` supplies a book per venue name. A venue whose book cannot supply the
/// size is still considered — its `Execution` reports `depth_exhausted` — but it
/// ranks behind any venue that can, because a partial fill at a good price is
/// not a fill.
///
/// This is the function the whole library builds toward: it answers what a
/// trade actually costs, on real depth, on a book proven correct against the
/// exchange's own checksum.
template <typename BookLookup>
[[nodiscard]] BestExecution best_execution(const ConsolidatedBook& consolidated, Side side,
                                           Qty wanted, Timestamp now, BookLookup&& books) {
    BestExecution best;
    std::int64_t best_effective = 0;
    bool best_complete = false;

    for (const VenueQuote& quote : consolidated.quotes()) {
        if (!consolidated.usable(quote, side, now)) {
            continue;
        }
        const auto* book = books(quote.venue);
        if (book == nullptr) {
            continue;
        }
        ++best.venues_considered;

        const Execution execution = cost_to_trade(*book, side, wanted);
        if (execution.empty()) {
            continue;
        }

        const std::int64_t adjustment = (execution.vwap.ticks * quote.fees.taker) / 1'000'000;
        const std::int64_t effective =
            side == Side::kAsk ? execution.vwap.ticks + adjustment
                               : execution.vwap.ticks - adjustment;
        const bool complete = !execution.depth_exhausted;

        // A venue that can fill the whole size always beats one that cannot,
        // regardless of price: the leftover has to be traded somewhere, and
        // pricing it as if it were free is how a router lies about its own cost.
        const bool better = !best.found || (complete && !best_complete) ||
                            (complete == best_complete &&
                             (side == Side::kAsk ? effective < best_effective
                                                 : effective > best_effective));
        if (better) {
            best.found = true;
            best.venue = quote.venue;
            best.execution = execution;
            best.effective_vwap = Price{effective};
            best_effective = effective;
            best_complete = complete;
        }
    }
    return best;
}

}  // namespace crossbook
