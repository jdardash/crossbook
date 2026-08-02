// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The README's two code blocks, end to end, as a program that is compiled by
// CI rather than read.
//
// A README code block is the first thing a reader runs and the last thing
// anybody checks. The blocks in this one referenced `crossbook/feed.hpp` and
// `crossbook/venues/kraken.hpp` at a tag where neither file existed, which is
// the failure mode uncompiled documentation always eventually reaches. This
// file exists so that the shape of the API a reader is shown -- construct a
// Feed, push frames at it, branch on FeedStatus, refuse to read the book until
// synced() -- cannot compile-rot without a build going red.
//
// It also runs. Every frame below is verified against Kraken's own CRC32,
// computed here from an independently built mirror book rather than
// hard-coded, so the example asserts the same thing the library claims instead
// of printing a number nobody checks. The process exit status is the assertion:
// anything unexpected and it is non-zero.
//
// Deliberately no network. The point of the correctness core is that it can be
// exercised without opening a socket, and an example that needs an exchange to
// be reachable is one CI has to be allowed to skip.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"
#include "crossbook/execution.hpp"
#include "crossbook/feed.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;

namespace {

using KrakenFeed = Feed<venues::KrakenBookDecoder, ArrayBook>;

// BTC/USD on Kraken: one decimal of price, eight of quantity. These scales are
// not cosmetic -- they define the mantissa, and the mantissa is what the
// exchange checksums.
InstrumentSpec btc_usd() {
    return InstrumentSpec{"BTC/USD", 1, 8};
}

struct WireLevel {
    const char* price;  // Exactly as the venue spells it, decimal point and all.
    const char* qty;
    std::int64_t price_ticks;
    std::int64_t qty_units;
};

// A shallow but realistic top of book. Two levels a side is enough to make
// executable_size() walk more than the touch, which is the whole reason that
// function exists.
constexpr WireLevel kAsks[] = {
    {"45283.6", "0.30000000", 452836, 30'000'000},
    {"45283.9", "1.25000000", 452839, 125'000'000},
};
constexpr WireLevel kBids[] = {
    {"45283.5", "0.50000000", 452835, 50'000'000},
    {"45283.1", "2.00000000", 452831, 200'000'000},
};

/// Build the book the snapshot below describes, independently of the decoder,
/// so the checksum the frame carries is derived rather than copied.
ArrayBook mirror_book() {
    ArrayBook book(btc_usd());
    for (const WireLevel& l : kAsks) {
        book.apply(Side::kAsk, Price{l.price_ticks}, Qty{l.qty_units});
    }
    for (const WireLevel& l : kBids) {
        book.apply(Side::kBid, Price{l.price_ticks}, Qty{l.qty_units});
    }
    return book;
}

std::string levels_json(const WireLevel* levels, std::size_t n) {
    std::string out = "[";
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) {
            out += ',';
        }
        out += "{\"price\":";
        out += levels[i].price;
        out += ",\"qty\":";
        out += levels[i].qty;
        out += '}';
    }
    out += ']';
    return out;
}

std::string kraken_frame(std::string_view type, std::uint32_t checksum) {
    std::string out = "{\"channel\":\"book\",\"type\":\"";
    out += type;
    out += "\",\"data\":[{\"symbol\":\"BTC/USD\",\"asks\":";
    out += levels_json(kAsks, sizeof(kAsks) / sizeof(kAsks[0]));
    out += ",\"bids\":";
    out += levels_json(kBids, sizeof(kBids) / sizeof(kBids[0]));
    out += ",\"checksum\":";
    out += std::to_string(checksum);
    out += "}]}";
    return out;
}

int fail(const char* what) {
    std::fprintf(stderr, "example: %s\n", what);
    return 1;
}

}  // namespace

int main() {
    // ---------------------------------------------------------------------
    // Drive the whole pipeline -- decode, verify, recover -- from raw frames.
    // ---------------------------------------------------------------------
    KrakenFeed feed("kraken", venues::KrakenBookDecoder(btc_usd()),
                    SequencePolicy::kStrictIncrement);

    const std::uint32_t checksum = kraken_checksum(mirror_book());
    const std::string snapshot = kraken_frame("snapshot", checksum);

    switch (feed.handle(snapshot)) {
        case FeedStatus::kApplied:
            break;  // Verified against Kraken's CRC32.
        case FeedStatus::kIgnored:
        case FeedStatus::kRejected:
        case FeedStatus::kNeedsSnapshot:
            return fail("the snapshot did not apply");
    }

    // A heartbeat is not an update and must not count as activity.
    if (feed.handle(R"({"channel":"heartbeat"})") != FeedStatus::kIgnored) {
        return fail("a heartbeat was mistaken for data");
    }

    // feed.synced() must be true before anyone reads the book.
    if (!feed.synced()) {
        return fail("synced() is false after a verified snapshot");
    }

    // ---------------------------------------------------------------------
    // What you can actually trade. The touch is not a size.
    // ---------------------------------------------------------------------
    Level touch{};
    if (!feed.book().best(Side::kAsk, touch)) {
        return fail("no best ask");
    }
    const Execution within_5bps = executable_size(feed.book(), Side::kAsk, from_bps(5));
    const Execution one_coin = cost_to_trade(feed.book(), Side::kAsk, Qty{100'000'000});

    std::printf("best ask        %lld ticks\n", static_cast<long long>(touch.price.ticks));
    std::printf("within 5 bps    qty %lld over %zu levels, vwap %lld, slippage %lld (0.01 bps)\n",
                static_cast<long long>(within_5bps.qty.units), within_5bps.levels,
                static_cast<long long>(within_5bps.vwap.ticks),
                static_cast<long long>(within_5bps.slippage));
    std::printf("one coin        qty %lld, vwap %lld, depth_exhausted %s\n",
                static_cast<long long>(one_coin.qty.units),
                static_cast<long long>(one_coin.vwap.ticks),
                one_coin.depth_exhausted ? "yes" : "no");

    // The touch alone would have claimed 0.3 was available at 45283.6. Walking
    // the book is the difference between a spread that looks profitable and one
    // that is.
    if (within_5bps.qty.units <= touch.qty.units) {
        return fail("executable_size found no more than the touch");
    }

    // ---------------------------------------------------------------------
    // A book that is known to be wrong is never served and never updated
    // further until it has been rebuilt from a snapshot.
    // ---------------------------------------------------------------------
    const std::string corrupted = kraken_frame("update", checksum ^ 0x1u);
    if (feed.handle(corrupted) != FeedStatus::kNeedsSnapshot) {
        return fail("a checksum mismatch did not stop the feed");
    }
    if (feed.synced()) {
        return fail("the feed kept serving a book it knows is wrong");
    }
    if (feed.divergences().count(DivergenceKind::kChecksumMismatch) != 1) {
        return fail("the mismatch was counted but not enumerated");
    }

    // Resubscribing is what recovery looks like from here.
    if (feed.handle(snapshot) != FeedStatus::kApplied) {
        return fail("the feed did not recover from a fresh snapshot");
    }

    // feed.match_rate() is only evidence if feed.divergences().verified() > 0.
    std::printf("verified        %llu\n",
                static_cast<unsigned long long>(feed.divergences().verified()));
    std::printf("match rate      %.6f%%\n", feed.match_rate() * 100.0);

    if (feed.divergences().verified() == 0) {
        return fail("nothing was verified, so the match rate means nothing");
    }
    return 0;
}
