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
//   5. SCALE AND SYMBOL. Added after an audit found it missing entirely, which
//      is the most expensive omission this file has had. A mantissa is only a
//      price relative to a scale: Kraken quotes BTC/USD at price_scale 1, so
//      45283.6 arrives as 452836, while Binance quotes the same instrument at
//      price_scale 2, so 45283.50 arrives as 4528350. Feed both into one
//      consolidated view without checking and the higher-scaled venue wins
//      every bid ranking and loses every ask ranking forever, `crossed()` fires
//      permanently, and — the part with no visible symptom at all — a quantity
//      scale mismatch silently corrupts the VWAP *weights* inside
//      `cost_to_trade`, so the reported cost of a trade is wrong in a way no
//      amount of staring at the output reveals. The symbol is the same hole
//      with a different cause: nothing checked `InstrumentSpec::symbol`, so a
//      feed-handler routing bug that drops an ETH quote into a BTC book
//      produced a confident, large, entirely fictional arbitrage. Both are now
//      rejected at `update()` and again inside `best_execution`.
//
// What this class does NOT do is route, split, or place orders. It reports what
// the market looks like. Deciding what to do about that is a strategy concern
// and lives outside this library — see the note on scope in the README.
//
// A NOTE ON WHICH WAY THINGS ROUND AND WHO WINS A TIE
//
// Every fee adjustment below rounds away from zero and every VWAP rounds
// against the taker (see execution.hpp). Truncation used to flatter every venue
// on every path — asks understated, bid proceeds overstated — which both
// inverted genuine rankings and, by collapsing distinct effective prices onto
// one integer, manufactured ties. Ties are therefore broken on venue name
// rather than on arrival order, because arrival order is per-process state and
// two processes reading the same market must route the same way. Determinism is
// a headline claim of this library; a comparator that consults insertion order
// quietly exempts the cross-venue layer from it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/execution.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

/// Taker fees for one venue, in basis points of notional.
///
/// Only taker fees appear here because this models crossing the spread. Maker
/// rebates would change the sign and belong to a different question — and a
/// negative `taker` is rejected by `ConsolidatedBook::update` rather than
/// quietly making an ask look cheaper than the venue is actually quoting it.
struct FeeSchedule {
    /// Hundredths of a basis point. Use from_bps(26) for a 26 bps taker fee.
    CentiBps taker{0};
};

/// One venue's contribution to the consolidated view.
struct VenueQuote {
    std::string venue;

    /// The instrument the mantissas below are expressed in. Not decoration:
    /// without it a `Level` is an integer with no meaning, and mixing two
    /// scales in one consolidated view is undetectable after the fact. It must
    /// match the consolidated book's spec exactly — symbol and both scales — or
    /// `update()` refuses the quote.
    InstrumentSpec spec{};

    Level bid{};
    Level ask{};
    bool has_bid{false};
    bool has_ask{false};

    /// LOCAL receive time, not the venue's own timestamp. Venue clocks are not
    /// comparable to each other; local time is.
    ///
    /// Zero means UNSTAMPED, and an unstamped quote is unusable. The previous
    /// guard treated `received_at > 0` as a precondition for *excluding* a
    /// quote, which inverted the default: a caller who forgot to stamp — much
    /// the likeliest mistake in caller code, since zero is the struct default —
    /// got a venue that was immortally fresh and could be reported as best
    /// forever, including one that had disconnected at session start.
    Timestamp received_at{0};

    FeeSchedule fees{};

    /// False when the venue has lost sync. A book that is known wrong must not
    /// contribute to a best-price calculation.
    bool synced{true};
};

/// What to do about age. Replaces an overloaded `max_age_ns == 0` sentinel.
///
/// The sentinel meant "disable filtering", which was documented and tested — but
/// it was implemented as `max_age_ns_ > 0`, so a *negative* max age disabled the
/// filter too, silently, on a configuration that reads like an unusually strict
/// one. Whether staleness is enforced is a policy question and now has its own
/// type; the duration is only a number.
enum class StalenessPolicy : std::uint8_t {
    /// Exclude venues older than `max_age_ns`, and exclude anything whose
    /// timestamp cannot be trusted. The default, and the only setting fit for
    /// live use.
    kEnforce = 0,
    /// Never exclude on age. For replaying a historical capture, where "now" is
    /// meaningless and every quote is by construction ancient.
    kDisabled,
};

/// A venue's price adjusted for what it actually costs to trade there.
struct EffectivePrice {
    /// A view into the owning ConsolidatedBook's quote list, not a copy. The
    /// copy cost one allocation per venue per call on the touch-read path —
    /// which `book.hpp` went to considerable trouble to make O(1) and
    /// allocation-free one layer down. INVALIDATED by any `update()`, `remove()`
    /// or destruction of the book, exactly like a reference into a vector.
    std::string_view venue;
    Price quoted{};
    /// Quoted price adjusted by the taker fee, in the direction that costs
    /// money: buys pay more, sells receive less. Rounded away from zero, so the
    /// adjustment is never understated.
    Price effective{};
    Qty size{};

    friend bool operator==(const EffectivePrice&, const EffectivePrice&) = default;
};

/// The outcome of asking which venue to trade a given size on.
struct BestExecution {
    /// A view into the ConsolidatedBook that produced this. See EffectivePrice.
    std::string_view venue;
    Execution execution{};
    /// VWAP after the venue's taker fee.
    Price effective_vwap{};
    /// How many venues were considered, after staleness and sync filtering.
    std::size_t venues_considered{0};
    /// How many venues were dropped after passing the quote-level filter —
    /// missing book, book on the wrong instrument, stale book, or no executable
    /// size. Previously every one of these was an invisible `continue`, so a
    /// router could pick a venue while silently discarding the two that
    /// disagreed with it about what instrument was being traded.
    std::size_t venues_rejected{0};
    bool found{false};
};

namespace detail {

/// Do two specs describe the same instrument at the same scales?
///
/// InstrumentSpec has no operator== and types.hpp is not ours to change, so the
/// comparison lives here. All three fields matter: two of them define what the
/// mantissas mean, and the third defines what they are mantissas *of*.
[[nodiscard]] inline bool same_instrument(const InstrumentSpec& a,
                                          const InstrumentSpec& b) noexcept {
    return a.price_scale == b.price_scale && a.qty_scale == b.qty_scale && a.symbol == b.symbol;
}

/// Adjust a quoted price by a taker fee, in the direction that costs money.
///
/// Returns false rather than a wrapped price: `price * taker` is an unguarded
/// int64 multiply at heart, and it used to be written out twice — once here and
/// once inline in `best_execution` — so a fix to one would have left the other
/// wrong. One definition, one guard, both call sites.
///
/// The adjustment rounds AWAY FROM ZERO. Truncating it understated the fee on
/// an ask (the venue looked cheaper to buy on than it was) and overstated the
/// proceeds on a bid (it looked like it paid more than it does), which is the
/// same direction twice: both push the consolidated market toward looking
/// crossed, and both flatter whichever venue happens to be quoting.
[[nodiscard]] inline bool apply_taker_fee(std::int64_t price, Side side, const FeeSchedule& fees,
                                          std::int64_t& out) noexcept {
    std::int64_t product = 0;
    if (!checked_mul_signed(price, fees.taker, product)) {
        return false;
    }
    const std::int64_t adjustment = div_away_from_zero(product, 1'000'000);
    // Lifting an ask costs the fee on top; hitting a bid nets it out.
    return side == Side::kAsk ? checked_add_signed(price, adjustment, out)
                              : checked_add_signed(price, -adjustment, out);
}

}  // namespace detail

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

    explicit ConsolidatedBook(InstrumentSpec spec, Timestamp max_age_ns = kDefaultMaxAgeNs,
                              StalenessPolicy staleness = StalenessPolicy::kEnforce)
        : spec_(std::move(spec)), max_age_ns_(max_age_ns), staleness_(staleness) {}

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] Timestamp max_age_ns() const noexcept { return max_age_ns_; }
    [[nodiscard]] StalenessPolicy staleness_policy() const noexcept { return staleness_; }

    /// Insert or replace a venue's quote. Returns false if the quote was
    /// refused, in which case the book is unchanged.
    ///
    /// Refusal is the point. A quote on a different instrument, at a different
    /// scale, or carrying a negative taker fee is not a quote this book can
    /// consolidate, and accepting it produces a confident wrong answer rather
    /// than a visible failure. Callers that ignore the return value at least
    /// see `rejected_updates()` climb.
    bool update(VenueQuote quote) {
        if (!detail::same_instrument(quote.spec, spec_)) {
            ++rejected_updates_;
            return false;
        }
        if (quote.fees.taker < 0) {
            // A maker rebate is a different question (see FeeSchedule). Applied
            // as a negative taker it would make an ask look cheaper than the
            // venue is quoting it, which is the one direction that costs money.
            ++rejected_updates_;
            return false;
        }
        for (VenueQuote& existing : quotes_) {
            if (existing.venue == quote.venue) {
                existing = std::move(quote);
                return true;
            }
        }
        quotes_.push_back(std::move(quote));
        return true;
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

    /// How many updates have been refused. A feed handler wiring the wrong
    /// instrument into this book shows up here rather than in the P&L.
    [[nodiscard]] std::size_t rejected_updates() const noexcept { return rejected_updates_; }

    /// Is a locally stamped timestamp fresh enough to act on as of `now`?
    ///
    /// Exposed because freshness has to be asked of more than the quote struct:
    /// `best_execution` walks a `BasicL2Book` the caller supplied separately,
    /// and that book has its own `last_update()`. See the note there.
    ///
    /// FAILS CLOSED, in all three directions the old inline guard failed open:
    ///   - unstamped (`<= 0`) is unusable, not immortal;
    ///   - a timestamp in the future is a clock fault, not eternal freshness —
    ///     the header promises local receive time and a venue clock running
    ///     fast would otherwise exempt that venue permanently;
    ///   - a non-positive `max_age_ns` under kEnforce excludes everything,
    ///     because "enforce an age limit of zero or less" cannot be satisfied
    ///     and must not silently mean "enforce nothing".
    ///
    /// An age exactly equal to `max_age_ns` is FRESH: the documented rule is
    /// that entries *older than* the limit are excluded, and the boundary has
    /// to fall on one side of that sentence deliberately rather than by
    /// whichever comparison operator got typed.
    [[nodiscard]] bool timestamp_fresh(Timestamp stamped_at, Timestamp now) const noexcept {
        if (staleness_ == StalenessPolicy::kDisabled) {
            return true;
        }
        if (stamped_at <= 0) {
            return false;
        }
        if (now < stamped_at) {
            return false;
        }
        if (max_age_ns_ <= 0) {
            return false;
        }
        return (now - stamped_at) <= max_age_ns_;
    }

    /// Is this venue currently usable as of `now`?
    ///
    /// Requires that it is synced, is quoting the instrument this book
    /// consolidates, has a quote on the side in question, and has been heard
    /// from recently.
    [[nodiscard]] bool usable(const VenueQuote& q, Side side, Timestamp now) const {
        if (!q.synced) {
            return false;
        }
        // Second line of defence behind update(). A VenueQuote can reach here
        // through quotes() and a caller-held copy, and the cost of the check is
        // a string compare against a mistake that is otherwise undetectable.
        if (!detail::same_instrument(q.spec, spec_)) {
            return false;
        }
        if (side == Side::kBid ? !q.has_bid : !q.has_ask) {
            return false;
        }
        return timestamp_fresh(q.received_at, now);
    }

    /// Every usable venue's price on `side`, adjusted for taker fees, best
    /// first.
    ///
    /// "Best" means best *effective* price. That ordering routinely differs
    /// from the raw-price ordering, which is the whole reason this returns
    /// effective prices rather than quotes.
    ///
    /// This allocates, and it is the only function here that does. Reading the
    /// touch does not — see `best()`.
    [[nodiscard]] std::vector<EffectivePrice> ranked(Side side, Timestamp now) const {
        std::vector<EffectivePrice> out;
        out.reserve(quotes_.size());

        for (const VenueQuote& q : quotes_) {
            if (!usable(q, side, now)) {
                continue;
            }
            const Level& level = (side == Side::kBid) ? q.bid : q.ask;
            std::int64_t effective = 0;
            if (!detail::apply_taker_fee(level.price.ticks, side, q.fees, effective)) {
                continue;  // A wrapped effective price is not a rank.
            }
            out.push_back(EffectivePrice{std::string_view{q.venue}, level.price, Price{effective},
                                         level.qty});
        }

        // Insertion sort over a total order. The comparison used to be strict on
        // effective price alone, which left the order of equal entries decided
        // by `quotes_` order — that is, by which venue's first update arrived
        // first, which is per-process state. Two processes reading the same
        // market would then route differently, and defect-3 truncation was
        // actively manufacturing the ties that exposed it.
        for (std::size_t i = 1; i < out.size(); ++i) {
            EffectivePrice key = out[i];
            std::size_t j = i;
            while (j > 0 && ranks_after(out[j - 1], key, side)) {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = key;
        }
        return out;
    }

    /// The best effective price on `side`, if any venue is usable.
    ///
    /// A linear scan, not `ranked().front()`. Sorting every venue and heap
    /// allocating a vector of strings to answer "what is the best price right
    /// now" is precisely the cost `book.hpp` documents at length having removed
    /// one layer down, and `spread()` calls this twice while `crossed()` calls
    /// `spread()`, so the waste compounded three deep on the hottest question
    /// the class answers. Allocation-free, and pinned as such by a test.
    [[nodiscard]] bool best(Side side, Timestamp now, EffectivePrice& out) const {
        bool found = false;
        EffectivePrice leader{};

        for (const VenueQuote& q : quotes_) {
            if (!usable(q, side, now)) {
                continue;
            }
            const Level& level = (side == Side::kBid) ? q.bid : q.ask;
            std::int64_t effective = 0;
            if (!detail::apply_taker_fee(level.price.ticks, side, q.fees, effective)) {
                continue;
            }
            const EffectivePrice candidate{std::string_view{q.venue}, level.price,
                                           Price{effective}, level.qty};
            if (!found || ranks_after(leader, candidate, side)) {
                leader = candidate;
                found = true;
            }
        }

        if (!found) {
            return false;
        }
        out = leader;
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
        return detail::checked_sub_signed(best_ask.effective.ticks, best_bid.effective.ticks,
                                          out);
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

        std::int64_t delta = 0;
        if (!detail::checked_sub_signed(best_ask.effective.ticks, best_bid.effective.ticks,
                                        delta)) {
            return false;
        }
        // `(bid + ask) / 2` is the textbook overflow, and on a two-venue book of
        // large mantissas it is reachable. Anchoring on the bid and adding half
        // the delta keeps every intermediate between the two operands.
        const std::int64_t mid = best_bid.effective.ticks + delta / 2;
        if (mid <= 0) {
            // A non-positive mid makes the ratio meaningless and, worse, a
            // negative one silently flips the sign of the answer: a healthy
            // positive spread would be reported as a crossed market.
            return false;
        }
        std::int64_t scaled = 0;
        if (!detail::checked_mul_signed(delta, 1'000'000, scaled)) {
            return false;
        }
        out = scaled / mid;
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
    /// Does `a` rank strictly behind `b` on `side`?
    ///
    /// Taking the ask means buying: a cheaper effective price is better. Taking
    /// the bid means selling: a higher effective price is better. Equal prices
    /// fall back to the venue name, so the answer never depends on the order
    /// updates happened to arrive in.
    [[nodiscard]] static bool ranks_after(const EffectivePrice& a, const EffectivePrice& b,
                                          Side side) noexcept {
        if (a.effective.ticks != b.effective.ticks) {
            return side == Side::kAsk ? (a.effective.ticks > b.effective.ticks)
                                      : (a.effective.ticks < b.effective.ticks);
        }
        return a.venue > b.venue;
    }

    InstrumentSpec spec_;
    Timestamp max_age_ns_;
    StalenessPolicy staleness_;
    std::vector<VenueQuote> quotes_;
    std::size_t rejected_updates_{0};
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
///
/// THREE THINGS IT CHECKS THAT ARE NOT OBVIOUS:
///
///   - The book's instrument. `consolidated` and `books` are populated on
///     separate paths, so nothing but this comparison stops a book quoting a
///     different symbol or a different price scale from being walked at full
///     size and winning on a mantissa that means something else entirely.
///
///   - The book's own age. Freshness used to be asked of the `VenueQuote` while
///     the data actually walked came from a `BasicL2Book` the caller supplied
///     separately — a staleness gate protecting a struct nobody trades against.
///     Since the header deliberately holds *copies* of each touch, updating
///     quotes and books on different paths is the natural design and the gap is
///     the natural bug. `BasicL2Book::last_update()` already existed and was
///     never consulted; it is now, under the same policy as the quote.
///
///   - The `Execution` status rather than just its emptiness. An overflowing
///     book and a limit that excluded everything are not "this venue has no
///     size"; they are refusals to answer, and they are counted in
///     `venues_rejected` rather than vanishing into a bare `continue`.
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
        if (!detail::same_instrument(book->spec(), consolidated.spec())) {
            ++best.venues_rejected;
            continue;
        }
        if (!consolidated.timestamp_fresh(book->last_update(), now)) {
            ++best.venues_rejected;
            continue;
        }
        ++best.venues_considered;

        const Execution execution = cost_to_trade(*book, side, wanted);
        if (!execution.ok()) {
            ++best.venues_rejected;
            continue;
        }

        std::int64_t effective = 0;
        if (!detail::apply_taker_fee(execution.vwap.ticks, side, quote.fees, effective)) {
            ++best.venues_rejected;
            continue;
        }
        const bool complete = !execution.depth_exhausted;

        // A venue that can fill the whole size always beats one that cannot,
        // regardless of price: the leftover has to be traded somewhere, and
        // pricing it as if it were free is how a router lies about its own cost.
        //
        // The final clause is the determinism tie-break. `effective` is a VWAP
        // rounded against the taker and then fee-adjusted away from zero, so two
        // genuinely different costs under a tick apart can land on the same
        // integer; leaving that to iteration order would make the route depend
        // on which venue connected first. Comparing exactly instead would need
        // to cross-multiply notional against quantity, and at realistic crypto
        // scales (~4.5e13 notional against a ~1e8 quantity mantissa) that
        // product is ~4.5e21 — past int64 on essentially every real book, so the
        // exact comparison would fall back to this one almost always and be
        // decoration. A deterministic tie-break is the honest version.
        bool better = false;
        if (!best.found) {
            better = true;
        } else if (complete != best_complete) {
            better = complete;
        } else if (effective != best_effective) {
            better = (side == Side::kAsk) ? (effective < best_effective)
                                          : (effective > best_effective);
        } else {
            better = std::string_view{quote.venue} < best.venue;
        }

        if (better) {
            best.found = true;
            best.venue = std::string_view{quote.venue};
            best.execution = execution;
            best.effective_vwap = Price{effective};
            best_effective = effective;
            best_complete = complete;
        }
    }
    return best;
}

}  // namespace crossbook
