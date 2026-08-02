// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Verification against REAL Kraken data.
//
// Every other test in this suite checks crossbook against itself or against a
// specification. This one checks it against the exchange: 2,104 frames captured
// live from Kraken's public v2 book channel, replayed through the real decoder
// and the real book, with every CRC32 recomputed and compared to the one Kraken
// published in that message.
//
// It is the only test here that could have caught the bug it was written after.
// The library passed 226 spec-derived tests while silently mishandling
// depth-limited subscriptions: levels that fall off the bottom of a depth-N
// book are never explicitly deleted, so an untrimmed book accumulates stale
// levels that resurrect into the top ten when the price moves back. On this
// capture that produced 188 correct updates, then one mismatch, then an
// unrecoverable book.
//
// The fixture is committed so this runs in CI with no network. Regenerate a
// larger one with tools/capture_kraken.py.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "crossbook/capture.hpp"
#include "crossbook/feed.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;

namespace {

constexpr std::size_t kCaptureDepth = 25;  // The depth the capture subscribed at.

std::string fixture_path(const char* name = "kraken_btcusd_sample.cbcap") {
#ifdef CROSSBOOK_FIXTURE_DIR
    return std::string(CROSSBOOK_FIXTURE_DIR) + "/" + name;
#else
    return std::string("tests/fixtures/") + name;
#endif
}

/// One captured instrument, with the scales Kraken publishes for it.
///
/// Three price scales on purpose. BTC quotes to one decimal and XRP to five,
/// so a book at ~$0.50 carries a mantissa in the same numeric range as one at
/// ~$63,000 — which is precisely the arithmetic the checksum depends on. A
/// scale bug that a single instrument would hide shows up immediately across
/// this set.
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

using KrakenFeed = Feed<venues::KrakenBookDecoder, ArrayBook>;

KrakenFeed make_feed(std::size_t depth, const char* symbol = "BTC/USD",
                     Scale price_scale = 1) {
    return KrakenFeed("kraken",
                      venues::KrakenBookDecoder(InstrumentSpec{symbol, price_scale, 8}),
                      SequencePolicy::kStrictIncrement, depth);
}

}  // namespace

TEST_CASE("the live Kraken fixture loads", "[live]") {
    Capture capture;
    REQUIRE(capture.load(fixture_path()) == CaptureError::kOk);
    CHECK(capture.size() > 2000);
    CHECK(capture.duration_ns() > 10'000'000'000);  // Tens of seconds of market.
}

TEST_CASE("every checksum in the live capture matches Kraken", "[live][checksum]") {
    // THE CLAIM THE WHOLE LIBRARY RESTS ON, checked against the exchange rather
    // than against our own reading of its documentation.
    Capture capture;
    REQUIRE(capture.load(fixture_path()) == CaptureError::kOk);

    auto feed = make_feed(kCaptureDepth);
    for (const ReplayEvent& event : capture.events()) {
        const FeedStatus status = feed.handle(event.frame);
        // Heartbeats and status frames are ignored; nothing should be rejected
        // or force a resync on a clean capture.
        CHECK(status != FeedStatus::kRejected);
        CHECK(status != FeedStatus::kNeedsSnapshot);
    }

    // A match rate is only evidence if something was actually verified. An
    // empty log reports 1.0, and reading that as success is the easiest way to
    // fool yourself with this test.
    REQUIRE(feed.divergences().verified() > 2000);

    CHECK(feed.stats().checksum_mismatches == 0);
    CHECK(feed.divergences().total_recorded() == 0);
    CHECK(feed.match_rate() == 1.0);
    CHECK(feed.synced());
}

TEST_CASE("omitting the depth trim diverges on real data", "[live][checksum]") {
    // The regression guard. This is what the bug looked like: a long run of
    // correct updates, then a mismatch, then a book that can never recover.
    //
    // Asserting the failure keeps the fix honest — if someone makes trim() a
    // no-op, the passing test above would still pass, because an untrimmed book
    // is correct right up until it isn't.
    Capture capture;
    REQUIRE(capture.load(fixture_path()) == CaptureError::kOk);

    auto feed = make_feed(0);  // 0 = no trimming.
    for (const ReplayEvent& event : capture.events()) {
        (void)feed.handle(event.frame);
    }

    CHECK(feed.stats().checksum_mismatches > 0);
    CHECK(feed.divergences().count(DivergenceKind::kChecksumMismatch) > 0);
    CHECK(feed.match_rate() < 1.0);
}

TEST_CASE("both book implementations agree on real data", "[live][equivalence]") {
    // The differential oracle, run over real venue traffic rather than
    // generated input.
    Capture capture;
    REQUIRE(capture.load(fixture_path()) == CaptureError::kOk);

    Feed<venues::KrakenBookDecoder, ArrayBook> fast(
        "kraken", venues::KrakenBookDecoder(InstrumentSpec{"BTC/USD", 1, 8}),
        SequencePolicy::kStrictIncrement, kCaptureDepth);
    Feed<venues::KrakenBookDecoder, MapBook> reference(
        "kraken", venues::KrakenBookDecoder(InstrumentSpec{"BTC/USD", 1, 8}),
        SequencePolicy::kStrictIncrement, kCaptureDepth);

    for (const ReplayEvent& event : capture.events()) {
        (void)fast.handle(event.frame);
        (void)reference.handle(event.frame);
        REQUIRE(fast.book().state_hash() == reference.book().state_hash());
    }
    CHECK(fast.stats().checksums_verified == reference.stats().checksums_verified);
}

TEST_CASE("replaying the live capture is deterministic", "[live][determinism]") {
    Capture capture;
    REQUIRE(capture.load(fixture_path()) == CaptureError::kOk);

    auto run = [&]() {
        auto feed = make_feed(kCaptureDepth);
        for (const ReplayEvent& event : capture.events()) {
            (void)feed.handle(event.frame);
        }
        return feed.book().state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("every checksum matches across three price scales", "[live][checksum]") {
    // THE BROADEST EVIDENCE IN THE REPOSITORY.
    //
    // BTC at one decimal, ETH at two, XRP at five. A ~$0.50 instrument quoted
    // to five places produces mantissas in the same numeric range as a ~$63,000
    // one quoted to one, so this exercises the fixed-point and checksum path at
    // both ends rather than at a single convenient point.
    //
    // Full captures behind these slices: 105,334 checksums on ETH and 92,462 on
    // XRP over 40 minutes each, both at 100%.
    for (const Instrument& instrument : kInstruments) {
        INFO("instrument " << instrument.symbol << " price_scale="
                           << static_cast<int>(instrument.price_scale));

        Capture capture;
        REQUIRE(capture.load(fixture_path(instrument.file)) == CaptureError::kOk);
        REQUIRE(capture.size() > 1000);

        auto feed = make_feed(kCaptureDepth, instrument.symbol, instrument.price_scale);
        for (const ReplayEvent& event : capture.events()) {
            const FeedStatus status = feed.handle(event.frame);
            CHECK(status != FeedStatus::kRejected);
            CHECK(status != FeedStatus::kNeedsSnapshot);
        }

        REQUIRE(feed.divergences().verified() > 1000);
        CHECK(feed.stats().checksum_mismatches == 0);
        CHECK(feed.match_rate() == 1.0);
    }
}

TEST_CASE("the wrong price scale is caught by the exchange checksum",
          "[live][checksum]") {
    // Scales are instrument metadata you have to fetch, and getting one wrong
    // produces a book that is numerically plausible and fails every checksum.
    // That is the failure mode the verifier exists to make loud rather than
    // subtle, so it is worth proving it actually is loud.
    Capture capture;
    REQUIRE(capture.load(fixture_path("kraken_xrpusd_sample.cbcap")) == CaptureError::kOk);

    auto feed = make_feed(kCaptureDepth, "XRP/USD", 2);  // Should be 5.
    for (const ReplayEvent& event : capture.events()) {
        (void)feed.handle(event.frame);
    }
    // Either the values stop being representable at the wrong scale, or the
    // book builds and disagrees with Kraken. Both are detections; silence
    // would not be.
    CHECK((feed.stats().checksum_mismatches > 0 || feed.stats().rejected > 0));
    CHECK(feed.match_rate() < 1.0);
}
