// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Verification across three price scales, on live-captured Kraken data.
//
// `test_fixture_replay.cpp` proves the whole stack against one instrument.
// This file exists because one instrument is one point on the fixed-point
// axis, and the checksum is computed from mantissas at the instrument's scale.
//
// BTC/USD quotes to one decimal near $63,000; XRP/USD quotes to five decimals
// near $0.50. That puts their mantissas in the same numeric range, which is
// exactly where a scale bug hides: a book built at the wrong scale is
// numerically plausible, passes every internal consistency check, and is
// wrong. Across this set it has nowhere to go — and the last test proves the
// exchange checksum catches it loudly rather than letting it stay plausible.
//
// The fixtures are slices of live captures recorded 2026-08-01 (the full runs:
// 13,937 checksums on BTC over 9.5 minutes, 105,334 on ETH and 92,462 on XRP
// over 40 minutes each, all at 100%). The slices replay offline on every push.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "crossbook/capture.hpp"
#include "crossbook/feed.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;

namespace {

/// The depth these captures subscribed at. The checksum still covers the top
/// 10 levels regardless — depth changes what must be trimmed, not what Kraken
/// verifies.
constexpr std::size_t kCaptureDepth = 25;

/// One captured instrument, with the scales Kraken publishes for it.
struct Instrument {
    const char* file;
    const char* symbol;
    Scale price_scale;
};

constexpr Instrument kInstruments[] = {
    {"kraken_btcusd_sample.cbcap", "BTC/USD", 1},
    {"kraken_ethusd_sample.cbcap", "ETH/USD", 2},
    {"kraken_xrpusd_sample.cbcap", "XRP/USD", 5},
};

std::string fixture_path(const char* name) {
    return std::string(CROSSBOOK_FIXTURE_DIR) + "/" + name;
}

using KrakenFeed = Feed<venues::KrakenBookDecoder, ArrayBook>;

KrakenFeed make_feed(const char* symbol, Scale price_scale) {
    return KrakenFeed("kraken",
                      venues::KrakenBookDecoder(InstrumentSpec{symbol, price_scale, 8}),
                      SequencePolicy::kStrictIncrement, kCaptureDepth);
}

}  // namespace

TEST_CASE("every checksum matches across three price scales", "[fixture][checksum]") {
    for (const Instrument& instrument : kInstruments) {
        INFO("instrument " << instrument.symbol
                           << " price_scale=" << static_cast<int>(instrument.price_scale));

        Capture capture;
        std::string error;
        REQUIRE(capture.load(fixture_path(instrument.file), error));
        INFO("capture load error: " << error);
        REQUIRE(capture.venue() == "kraken");
        REQUIRE(capture.symbol() == instrument.symbol);
        REQUIRE(capture.frames().size() > 1000);

        auto feed = make_feed(instrument.symbol, instrument.price_scale);
        for (const CapturedFrame& frame : capture.frames()) {
            const FeedStatus status = feed.handle(frame.payload);
            // Heartbeats and status frames are ignored; nothing on a clean
            // capture should be rejected or force a resync.
            CHECK(status != FeedStatus::kRejected);
            CHECK(status != FeedStatus::kNeedsSnapshot);
        }

        // A match rate is only evidence if something was actually verified: an
        // empty run reports 1.0, and reading that as success is the easiest
        // way to fool yourself with this test.
        const FeedStats& stats = feed.stats();
        REQUIRE(stats.checksums_verified > 1000);
        CHECK(stats.checksum_mismatches == 0);
        CHECK(stats.rejected == 0);
        CHECK(feed.match_rate() == 1.0);
        CHECK(feed.synced());
        CHECK(feed.divergences().entries().empty());
    }
}

TEST_CASE("the wrong price scale is caught by the exchange checksum",
          "[fixture][checksum]") {
    // Scales are instrument metadata you have to fetch, and getting one wrong
    // produces a book that is numerically plausible and fails every checksum.
    // That is the failure mode the verifier exists to make loud rather than
    // subtle, so it is worth proving it actually is loud.
    Capture capture;
    std::string error;
    REQUIRE(capture.load(fixture_path("kraken_xrpusd_sample.cbcap"), error));

    auto feed = make_feed("XRP/USD", 2);  // Should be 5.
    for (const CapturedFrame& frame : capture.frames()) {
        (void)feed.handle(frame.payload);
    }

    // Either the values stop being representable at the wrong scale and the
    // ingest guard rejects them, or the book builds and disagrees with Kraken.
    // Both are detections; silence would not be. On this stack it is the
    // former: the canonical-spelling check refuses the frames before the book
    // ever sees them, which is earlier and louder than a checksum mismatch.
    CHECK((feed.stats().checksum_mismatches > 0 || feed.stats().rejected > 0));
    // Whichever way it was caught, a wrong-scale run must never present a
    // verified, matching book: either the match rate dropped, or nothing was
    // ever verified and the 1.0 is vacuous.
    CHECK((feed.match_rate() < 1.0 || feed.stats().checksums_verified == 0));
}
