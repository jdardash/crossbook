// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The end-to-end check: a recorded minute of Kraken, replayed offline.
//
// WHY THIS TEST IS THE MOST IMPORTANT ONE IN THE SUITE
//
// Every other test here checks a component against a rule I wrote down. This one
// checks the whole stack against the exchange's own arithmetic, on bytes the
// exchange actually sent, and it does it without a network so it runs on every
// push on every platform.
//
// `tests/fixtures/kraken_btcusd_l2.cbcap` is a verbatim recording of the Kraken v2
// book channel: no re-encoding, no normalisation, the same bytes crossbook_verify
// received live. Replaying it must reproduce, exactly:
//
//   - every one of the 301 checksums Kraken published, matched
//   - a book holding exactly the subscribed depth
//   - one specific state hash, identical on Windows, Linux and macOS
//
// That last one is the determinism claim. There is no floating point anywhere in
// the book, so there is nothing left that could legitimately differ between
// platforms — if this hash moves, something is wrong, and the test says so
// rather than leaving it to be noticed in production.
//
// The capture is 72 KB and committed. That is the whole point: in equities the
// equivalent data cannot be redistributed, so public order book projects ship
// without runnable data and ask to be believed. Crypto venues impose no such
// restriction, so the claim can simply be checked.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "crossbook/capture.hpp"
#include "crossbook/feed.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;

namespace {

constexpr const char* kFixture = CROSSBOOK_FIXTURE_DIR "/kraken_btcusd_l2.cbcap";

/// Scales and depth as Kraken spelled them in this capture. Hard-coded here
/// rather than inferred, so the test pins the values instead of agreeing with
/// whatever the inference happens to produce.
constexpr Scale kPriceScale = 1;
constexpr Scale kQtyScale = 8;
constexpr std::size_t kDepth = 10;

}  // namespace

TEST_CASE("The recorded Kraken capture replays with every checksum matching", "[fixture]") {
    Capture capture;
    std::string error;
    REQUIRE(capture.load(kFixture, error));
    INFO("capture load error: " << error);
    REQUIRE(capture.venue() == "kraken");
    REQUIRE(capture.symbol() == "BTC/USD");
    REQUIRE(capture.frames().size() > 300);

    Feed<venues::KrakenBookDecoder> feed(
        "kraken", venues::KrakenBookDecoder(InstrumentSpec{"BTC/USD", kPriceScale, kQtyScale}),
        SequencePolicy::kStrictIncrement, kDepth);

    for (const CapturedFrame& frame : capture.frames()) {
        (void)feed.handle(frame.payload);
    }

    const FeedStats& stats = feed.stats();

    // The headline claim, as an assertion rather than a README sentence.
    CHECK(stats.checksum_mismatches == 0);
    CHECK(stats.rejected == 0);
    CHECK(stats.resyncs_requested == 0);
    CHECK(feed.synced());

    // Verifying nothing would satisfy every check above, so require that real
    // verification happened.
    CHECK(stats.checksums_verified == 301);
    CHECK(feed.match_rate() == 1.0);
    CHECK(feed.divergences().entries().empty());

    // The depth contract: a top-10 subscription holds ten levels per side, no
    // matter how many distinct prices passed through the window.
    CHECK(feed.book().bids().size() == kDepth);
    CHECK(feed.book().asks().size() == kDepth);
    CHECK(stats.levels_trimmed > 0);
}

TEST_CASE("Replaying the capture is bit-identical across platforms", "[fixture][determinism]") {
    Capture capture;
    std::string error;
    REQUIRE(capture.load(kFixture, error));

    Feed<venues::KrakenBookDecoder> feed(
        "kraken", venues::KrakenBookDecoder(InstrumentSpec{"BTC/USD", kPriceScale, kQtyScale}),
        SequencePolicy::kStrictIncrement, kDepth);
    for (const CapturedFrame& frame : capture.frames()) {
        (void)feed.handle(frame.payload);
    }

    // FNV-1a over the mantissas of every level, in book order. No floating
    // point is involved anywhere upstream of this, so a difference here is a
    // defect and never a rounding difference.
    CHECK(feed.book().state_hash() == 0x080281c2dd87183fULL);
}

TEST_CASE("Without depth trimming the same capture fails, which is why trimming exists",
          "[fixture][trim]") {
    // A negative control. If a future change made trimming unnecessary — or made
    // it a no-op — this test would start passing for the wrong reason, and the
    // check above would no longer be evidence of anything.
    Capture capture;
    std::string error;
    REQUIRE(capture.load(kFixture, error));

    Feed<venues::KrakenBookDecoder> feed(
        "kraken", venues::KrakenBookDecoder(InstrumentSpec{"BTC/USD", kPriceScale, kQtyScale}),
        SequencePolicy::kStrictIncrement, /*depth=*/0);
    for (const CapturedFrame& frame : capture.frames()) {
        (void)feed.handle(frame.payload);
    }

    CHECK(feed.stats().checksum_mismatches > 0);
    CHECK(feed.book().bids().size() > kDepth);
}
