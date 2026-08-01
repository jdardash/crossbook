// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The recovery state machine.
//
// The property under test throughout is not "does it notice a problem" but
// "does it refuse to keep going after noticing". A feed handler that detects a
// gap and then applies the next update produces a book that looks entirely
// plausible and is wrong in ways nothing downstream can see.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "crossbook/feed.hpp"
#include "crossbook/venues/binance.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;
using namespace crossbook::venues;

namespace {

using KrakenFeed = Feed<KrakenBookDecoder, ArrayBook>;
using BinanceFeed = Feed<BinanceDepthDecoder, ArrayBook>;

KrakenFeed make_kraken() {
    return KrakenFeed("kraken", KrakenBookDecoder(InstrumentSpec{"BTC/USD", 1, 8}),
                      SequencePolicy::kStrictIncrement);
}

BinanceFeed make_binance(BinanceMarket market = BinanceMarket::kSpot) {
    BinanceDepthDecoder decoder(InstrumentSpec{"BTCUSDT", 2, 8}, market);
    const SequencePolicy policy = decoder.policy();
    return BinanceFeed("binance", std::move(decoder), policy);
}

/// A Kraken snapshot whose checksum is left for the caller to fill in.
std::string kraken_snapshot(std::uint32_t checksum) {
    return std::string(R"({"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD",)") +
           R"("asks":[{"price":45283.6,"qty":0.30000000}],)" +
           R"("bids":[{"price":45283.5,"qty":0.50000000}],)" + R"("checksum":)" +
           std::to_string(checksum) + "}]}";
}

/// The checksum Kraken would publish for the book that snapshot produces.
std::uint32_t expected_snapshot_checksum() {
    ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});
    book.apply(Side::kAsk, Price{452836}, Qty{30'000'000});
    book.apply(Side::kBid, Price{452835}, Qty{50'000'000});
    return kraken_checksum(book);
}

std::string binance_update(std::uint64_t first, std::uint64_t final_id,
                           std::string_view price = "45283.50",
                           std::string_view qty = "0.50000000") {
    return std::string(R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":)") + std::to_string(first) +
           R"(,"u":)" + std::to_string(final_id) + R"(,"b":[[")" + std::string(price) + R"(",")" +
           std::string(qty) + R"("]],"a":[]})";
}

}  // namespace

// ---------------------------------------------------------------------------
// The core safety property
// ---------------------------------------------------------------------------

TEST_CASE("a feed serves nothing until it has a snapshot", "[feed]") {
    auto feed = make_binance();
    CHECK_FALSE(feed.synced());
    CHECK(feed.handle(binance_update(1, 2)) == FeedStatus::kNeedsSnapshot);
    CHECK_FALSE(feed.synced());
    CHECK(feed.book().bids().size() == 0);
}

TEST_CASE("a gap stops the feed and keeps it stopped", "[feed]") {
    // The failure that costs money is not the dropped message. It is applying
    // the next one anyway.
    auto feed = make_binance();

    const std::string snapshot =
        R"({"lastUpdateId":100,"bids":[["45283.50","1.00000000"]],"asks":[]})";
    const DecodedMessage& snap = feed.decoder().decode_snapshot(snapshot);
    REQUIRE(snap.ok());
    REQUIRE(feed.apply_snapshot(snap) == FeedStatus::kApplied);
    REQUIRE(feed.synced());

    REQUIRE(feed.handle(binance_update(98, 105)) == FeedStatus::kApplied);

    // Drop update 106: jump straight to 108.
    CHECK(feed.handle(binance_update(108, 110)) == FeedStatus::kNeedsSnapshot);
    CHECK_FALSE(feed.synced());

    // Everything after must also refuse, however well-formed and contiguous
    // with each other those later messages are.
    CHECK(feed.handle(binance_update(111, 112)) == FeedStatus::kNeedsSnapshot);
    CHECK(feed.handle(binance_update(113, 114)) == FeedStatus::kNeedsSnapshot);
    CHECK_FALSE(feed.synced());

    CHECK(feed.divergences().count(DivergenceKind::kSequenceGap) == 1);
    CHECK(feed.stats().resyncs_requested >= 1);
}

TEST_CASE("a fresh snapshot restores service after a gap", "[feed]") {
    auto feed = make_binance();
    const DecodedMessage& first = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":100,"bids":[["45283.50","1.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(first) == FeedStatus::kApplied);
    REQUIRE(feed.handle(binance_update(98, 105)) == FeedStatus::kApplied);
    REQUIRE(feed.handle(binance_update(108, 110)) == FeedStatus::kNeedsSnapshot);

    const DecodedMessage& second = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":200,"bids":[["45283.50","2.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(second) == FeedStatus::kApplied);
    CHECK(feed.synced());
    CHECK(feed.handle(binance_update(198, 205)) == FeedStatus::kApplied);
}

TEST_CASE("a snapshot replaces the book rather than merging into it", "[feed]") {
    // Applying a snapshot onto stale state leaves levels the venue has no
    // reason to ever delete, because it does not know you invented them.
    auto feed = make_binance();
    const DecodedMessage& first = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":100,"bids":[["100.00","1.00000000"],["99.00","1.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(first) == FeedStatus::kApplied);
    REQUIRE(feed.book().bids().size() == 2);

    const DecodedMessage& second = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":200,"bids":[["50.00","1.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(second) == FeedStatus::kApplied);

    CHECK(feed.book().bids().size() == 1);  // Not 3.
    Level best{};
    REQUIRE(feed.book().best(Side::kBid, best));
    CHECK(best.price.ticks == 5000);
}

TEST_CASE("stale duplicates are discarded without breaking sync", "[feed]") {
    // Normal while draining the buffer captured during a snapshot fetch. These
    // must not be mistaken for gaps.
    auto feed = make_binance();
    const DecodedMessage& snap = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":100,"bids":[["45283.50","1.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(snap) == FeedStatus::kApplied);

    CHECK(feed.handle(binance_update(90, 95)) == FeedStatus::kIgnored);
    CHECK(feed.synced());
    CHECK(feed.divergences().count(DivergenceKind::kSequenceGap) == 0);
    CHECK(feed.handle(binance_update(98, 105)) == FeedStatus::kApplied);
}

// ---------------------------------------------------------------------------
// Checksum verification
// ---------------------------------------------------------------------------

TEST_CASE("a correct Kraken checksum verifies", "[feed][kraken]") {
    auto feed = make_kraken();
    const std::uint32_t good = expected_snapshot_checksum();

    CHECK(feed.handle(kraken_snapshot(good)) == FeedStatus::kApplied);
    CHECK(feed.synced());
    CHECK(feed.stats().checksums_verified == 1);
    CHECK(feed.stats().checksum_mismatches == 0);
    CHECK(feed.match_rate() == 1.0);
}

TEST_CASE("a wrong Kraken checksum forces a resnapshot", "[feed][kraken]") {
    // The book disagrees with the exchange's own arithmetic. Continuing to
    // serve it is the one thing that must not happen.
    auto feed = make_kraken();
    CHECK(feed.handle(kraken_snapshot(0xDEADBEEF)) == FeedStatus::kNeedsSnapshot);
    CHECK_FALSE(feed.synced());
    CHECK(feed.stats().checksum_mismatches == 1);
    CHECK(feed.divergences().count(DivergenceKind::kChecksumMismatch) == 1);
}

TEST_CASE("a checksum mismatch records the payload that produced it", "[feed][kraken]") {
    // "expected X, got Y" without the input is not debuggable at 3am.
    auto feed = make_kraken();
    (void)feed.handle(kraken_snapshot(0xDEADBEEF));

    REQUIRE(feed.divergences().entries().size() == 1);
    const Divergence& d = feed.divergences().entries().front();
    CHECK(d.kind == DivergenceKind::kChecksumMismatch);
    CHECK(d.venue == "kraken");
    CHECK(d.expected == 0xDEADBEEFULL);
    CHECK(d.actual == expected_snapshot_checksum());
    CHECK_FALSE(d.detail.empty());  // The checksum payload.
    CHECK(d.detail.find("452836") != std::string::npos);
}

TEST_CASE("match rate reflects verified updates", "[feed][kraken]") {
    auto feed = make_kraken();
    const std::uint32_t good = expected_snapshot_checksum();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(feed.handle(kraken_snapshot(good)) == FeedStatus::kApplied);
    }
    CHECK(feed.divergences().verified() == 4);
    CHECK(feed.match_rate() == 1.0);

    (void)feed.handle(kraken_snapshot(0xBADBAD));
    CHECK(feed.match_rate() < 1.0);
    CHECK(feed.match_rate() > 0.7);
}

TEST_CASE("an empty match rate is not evidence of correctness", "[feed]") {
    // A fresh log reports 1.0 because nothing has failed. Callers must check
    // that something was actually verified before believing it.
    auto feed = make_binance();
    CHECK(feed.match_rate() == 1.0);
    CHECK(feed.divergences().verified() == 0);  // The number that matters.
}

// ---------------------------------------------------------------------------
// Malformed input and staleness
// ---------------------------------------------------------------------------

TEST_CASE("a malformed frame is rejected without disturbing the book", "[feed]") {
    // A garbled frame says nothing about whether the book is still correct, so
    // it must not trigger an unnecessary resync — but it must be recorded.
    auto feed = make_binance();
    const DecodedMessage& snap = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":100,"bids":[["45283.50","1.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(snap) == FeedStatus::kApplied);
    const std::uint64_t hash_before = feed.book().state_hash();

    CHECK(feed.handle(R"({"e":"depthUpdate","U":1,"u":2,"b":[[)") == FeedStatus::kRejected);
    CHECK(feed.book().state_hash() == hash_before);
    CHECK(feed.synced());
    CHECK(feed.stats().rejected == 1);
}

TEST_CASE("a precision change is recorded with the offending token", "[feed]") {
    auto feed = make_kraken();
    REQUIRE(feed.handle(kraken_snapshot(expected_snapshot_checksum())) == FeedStatus::kApplied);

    // price_scale is 1; this update carries two decimals.
    constexpr std::string_view frame =
        R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
        R"("bids":[{"price":45283.55,"qty":0.5}]}]})";
    CHECK(feed.handle(frame) == FeedStatus::kRejected);
    CHECK(feed.divergences().count(DivergenceKind::kPrecisionLoss) == 1);
    CHECK(feed.divergences().entries().back().detail == "45283.55");
}

TEST_CASE("heartbeats are ignored and do not count as activity", "[feed]") {
    auto feed = make_kraken();
    CHECK(feed.handle(R"({"channel":"heartbeat"})") == FeedStatus::kIgnored);
    CHECK(feed.stats().ignored == 1);
    CHECK_FALSE(feed.synced());
}

TEST_CASE("a silent feed goes stale", "[feed]") {
    // A quiet market and a dead socket look identical from the book alone.
    // Serving a ten-minute-old book as current is its own kind of wrong.
    auto feed = make_binance();
    const DecodedMessage& snap = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":100,"bids":[["45283.50","1.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(snap) == FeedStatus::kApplied);
    REQUIRE(feed.handle(binance_update(98, 105)) == FeedStatus::kApplied);

    const Timestamp last = feed.book().last_update();
    CHECK_FALSE(feed.check_staleness(last + 1'000'000, 5'000'000'000));  // 1ms later: fine.
    CHECK(feed.check_staleness(last + 60'000'000'000, 5'000'000'000));   // 60s later: stale.
    CHECK_FALSE(feed.synced());
    CHECK(feed.divergences().count(DivergenceKind::kStaleFeed) == 1);
}

TEST_CASE("invalidate forces recovery on reconnect", "[feed]") {
    auto feed = make_binance();
    const DecodedMessage& snap = feed.decoder().decode_snapshot(
        R"({"lastUpdateId":100,"bids":[["45283.50","1.00000000"]],"asks":[]})");
    REQUIRE(feed.apply_snapshot(snap) == FeedStatus::kApplied);

    feed.invalidate();
    CHECK_FALSE(feed.synced());
    CHECK(feed.handle(binance_update(106, 110)) == FeedStatus::kNeedsSnapshot);
}

TEST_CASE("futures and spot feeds disagree on the same frame", "[feed][binance]") {
    // End-to-end confirmation that the continuity contracts really are
    // different, carried all the way from the decoder into the feed.
    const std::string frame =
        R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":999,"u":110,"pu":105,)"
        R"("b":[["45283.50","1.00000000"]],"a":[]})";
    const std::string snapshot =
        R"({"lastUpdateId":100,"bids":[["45283.50","1.00000000"]],"asks":[]})";

    auto spot = make_binance(BinanceMarket::kSpot);
    const DecodedMessage& s1 = spot.decoder().decode_snapshot(snapshot);
    REQUIRE(spot.apply_snapshot(s1) == FeedStatus::kApplied);
    REQUIRE(spot.handle(binance_update(98, 105)) == FeedStatus::kApplied);
    // Spot requires U == 106; it is 999.
    CHECK(spot.handle(frame) == FeedStatus::kNeedsSnapshot);

    auto futures = make_binance(BinanceMarket::kFutures);
    const DecodedMessage& s2 = futures.decoder().decode_snapshot(snapshot);
    REQUIRE(futures.apply_snapshot(s2) == FeedStatus::kApplied);
    REQUIRE(futures.handle(
                R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":98,"u":105,"pu":97,)"
                R"("b":[["45283.50","1.00000000"]],"a":[]})") == FeedStatus::kApplied);
    // Futures only requires pu == 105, which holds.
    CHECK(futures.handle(frame) == FeedStatus::kApplied);
}

TEST_CASE("stats account for every frame", "[feed]") {
    auto feed = make_kraken();
    (void)feed.handle(R"({"channel":"heartbeat"})");
    (void)feed.handle(kraken_snapshot(expected_snapshot_checksum()));
    (void)feed.handle(R"(not json at all)");

    const FeedStats& s = feed.stats();
    CHECK(s.frames == 3);
    CHECK(s.ignored == 1);
    CHECK(s.applied == 1);
    CHECK(s.rejected == 1);
    CHECK(s.snapshots_applied == 1);
}
