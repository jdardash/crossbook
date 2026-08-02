// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The Binance SBE depth decoder, against frames built byte-by-byte from the
// schema. No captured fixtures exist yet — the endpoint requires an Ed25519
// API key even for public data — so the fixtures here are hand-encoded from
// stream_1_0.xml, which has the useful property that every offset in the
// decoder was reached by an independent reading of the same document.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "crossbook/feed.hpp"
#include "crossbook/venues/binance_sbe.hpp"

using namespace crossbook;
using namespace crossbook::venues;

namespace {

/// Little-endian byte appender: the schema's byteOrder, spelled explicitly so
/// the fixture cannot silently inherit the host's.
struct Wire {
    std::string bytes;

    void u8(std::uint8_t v) { bytes.push_back(static_cast<char>(v)); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFF));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void i8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }
    void i64(std::int64_t v) {
        auto u = static_cast<std::uint64_t>(v);
        for (int i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>(u & 0xFF));
            u >>= 8;
        }
    }
    void str8(std::string_view s) {
        u8(static_cast<std::uint8_t>(s.size()));
        bytes.append(s);
    }
};

struct Level {
    std::int64_t price;
    std::int64_t qty;
};

/// A DepthDiffStreamEvent (template 10003), schema 1:0.
std::string diff_frame(std::int64_t event_us, std::int64_t first, std::int64_t last,
                       std::int8_t price_exp, std::int8_t qty_exp, const std::vector<Level>& bids,
                       const std::vector<Level>& asks, std::string_view symbol = "BTCUSDT") {
    Wire w;
    w.u16(26);     // blockLength
    w.u16(10003);  // templateId
    w.u16(1);      // schemaId
    w.u16(0);      // version
    w.i64(event_us);
    w.i64(first);
    w.i64(last);
    w.i8(price_exp);
    w.i8(qty_exp);
    for (const auto& side : {bids, asks}) {
        w.u16(16);  // group blockLength
        w.u16(static_cast<std::uint16_t>(side.size()));
        for (const Level& level : side) {
            w.i64(level.price);
            w.i64(level.qty);
        }
    }
    w.str8(symbol);
    return w.bytes;
}

/// A DepthSnapshotStreamEvent (template 10002), schema 1:0.
std::string snapshot_frame(std::int64_t event_us, std::int64_t book_id, std::int8_t price_exp,
                           std::int8_t qty_exp, const std::vector<Level>& bids,
                           const std::vector<Level>& asks, std::string_view symbol = "BTCUSDT") {
    Wire w;
    w.u16(18);
    w.u16(10002);
    w.u16(1);
    w.u16(0);
    w.i64(event_us);
    w.i64(book_id);
    w.i8(price_exp);
    w.i8(qty_exp);
    for (const auto& side : {bids, asks}) {
        w.u16(16);
        w.u16(static_cast<std::uint16_t>(side.size()));
        for (const Level& level : side) {
            w.i64(level.price);
            w.i64(level.qty);
        }
    }
    w.str8(symbol);
    return w.bytes;
}

BinanceSbeDecoder btcusdt() {
    return BinanceSbeDecoder(InstrumentSpec{"BTCUSDT", 2, 8});
}

}  // namespace

TEST_CASE("an SBE depth diff decodes to the instrument's grid", "[venues][binance][sbe]") {
    auto decoder = btcusdt();
    // price 45283.50 spelled as 4528350e-2; qty 0.5 as 50000000e-8.
    const std::string frame = diff_frame(1'700'000'000'000'000, 100, 105, -2, -8,
                                         {{4'528'350, 50'000'000}}, {{4'528'360, 30'000'000}});

    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kUpdate);
    CHECK(msg.symbol == "BTCUSDT");
    CHECK(msg.ts == 1'700'000'000'000'000'000);  // us in, ns out.
    REQUIRE(msg.has_ids);
    CHECK(msg.ids.first_id == 100);
    CHECK(msg.ids.final_id == 105);
    REQUIRE(msg.levels.size() == 2);
    CHECK(msg.levels[0].side == Side::kBid);
    CHECK(msg.levels[0].price.ticks == 4'528'350);
    CHECK(msg.levels[0].qty.units == 50'000'000);
    CHECK(msg.levels[1].side == Side::kAsk);
    CHECK(msg.levels[1].price.ticks == 4'528'360);
}

TEST_CASE("a coarser exponent rescales exactly", "[venues][binance][sbe]") {
    // The venue is free to send 45283.5 as 452835e-1; at price scale 2 that is
    // ticks 4528350 — an exact multiply, not a spelling problem, because the
    // binary wire has no spelling.
    auto decoder = btcusdt();
    const std::string frame = diff_frame(1, 100, 105, -1, -8, {{452'835, 50'000'000}}, {});
    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());
    REQUIRE(msg.levels.size() == 1);
    CHECK(msg.levels[0].price.ticks == 4'528'350);
}

TEST_CASE("a value off the instrument's grid is refused, not rounded", "[venues][binance][sbe]") {
    // 45283.5001 at price scale 2 would need rounding. Same contract as the
    // text path: kPrecisionLoss, and no levels survive.
    auto decoder = btcusdt();
    const std::string frame = diff_frame(1, 100, 105, -4, -8, {{452'835'001, 50'000'000}}, {});
    const DecodedMessage& msg = decoder.decode(frame);
    CHECK_FALSE(msg.ok());
    CHECK(msg.error == DecodeError::kPrecisionLoss);
    CHECK(msg.levels.empty());
}

TEST_CASE("a foreign symbol is ignored before any level is trusted", "[venues][binance][sbe]") {
    auto decoder = btcusdt();
    const std::string frame =
        diff_frame(1, 100, 105, -2, -8, {{4'528'350, 50'000'000}}, {}, "ETHUSDT");
    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kIgnored);
    CHECK(msg.levels.empty());
}

TEST_CASE("an SBE depth snapshot carries its book id as both sequence ends",
          "[venues][binance][sbe]") {
    auto decoder = btcusdt();
    const std::string frame =
        snapshot_frame(1, 5000, -2, -8, {{4'528'350, 50'000'000}}, {{4'528'360, 30'000'000}});
    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kSnapshot);
    CHECK(msg.ids.first_id == 5000);
    CHECK(msg.ids.final_id == 5000);
    REQUIRE(msg.levels.size() == 2);
}

TEST_CASE("other templates on the socket are routing, not failure", "[venues][binance][sbe]") {
    auto decoder = btcusdt();
    Wire w;
    w.u16(34);     // BestBidAskStreamEvent root: 8+8+1+1+8+8 exceeds; size irrelevant
    w.u16(10001);  // bestBidAsk template
    w.u16(1);
    w.u16(0);
    const DecodedMessage& msg = decoder.decode(w.bytes);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kIgnored);
}

TEST_CASE("a foreign schema id is loudly malformed", "[venues][binance][sbe]") {
    auto decoder = btcusdt();
    Wire w;
    w.u16(26);
    w.u16(10003);
    w.u16(2);  // Not the schema this decoder reads.
    w.u16(0);
    const DecodedMessage& msg = decoder.decode(w.bytes);
    CHECK_FALSE(msg.ok());
    CHECK(msg.error == DecodeError::kMalformed);
}

TEST_CASE("truncation anywhere is malformed, never a partial decode", "[venues][binance][sbe]") {
    auto decoder = btcusdt();
    const std::string whole =
        diff_frame(1, 100, 105, -2, -8, {{4'528'350, 50'000'000}}, {{4'528'360, 30'000'000}});
    // Every prefix must refuse. The last byte index is the full frame, which
    // must decode — pinning that the loop bound itself is right.
    for (std::size_t cut = 0; cut < whole.size(); ++cut) {
        const DecodedMessage& msg = decoder.decode(std::string_view(whole).substr(0, cut));
        INFO("cut=" << cut);
        REQUIRE((msg.error == DecodeError::kMalformed || msg.kind == MessageKind::kIgnored));
        REQUIRE(msg.levels.empty());
    }
    REQUIRE(decoder.decode(whole).ok());
}

TEST_CASE("a newer minor version's extra bytes are skipped, not misread",
          "[venues][binance][sbe]") {
    // Forward compatibility is the point of SBE blockLengths: a schema 1:1
    // that appends a root field and an entry field must still decode as 1:0.
    Wire w;
    w.u16(30);  // Root grew by 4 bytes.
    w.u16(10003);
    w.u16(1);
    w.u16(1);    // version 1
    w.i64(1);    // eventTime
    w.i64(100);  // first
    w.i64(105);  // last
    w.i8(-2);
    w.i8(-8);
    w.u16(0xBEEF);  // The appended root field this decoder has never heard of.
    w.u16(0xDEAD);
    w.u16(20);  // Entries grew by 4 bytes too.
    w.u16(1);
    w.i64(4'528'350);
    w.i64(50'000'000);
    w.u16(0xAAAA);  // Appended entry field.
    w.u16(0xBBBB);
    w.u16(20);  // Empty ask group, same stride.
    w.u16(0);
    w.str8("BTCUSDT");

    auto decoder = btcusdt();
    const DecodedMessage& msg = decoder.decode(w.bytes);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kUpdate);
    REQUIRE(msg.levels.size() == 1);
    CHECK(msg.levels[0].price.ticks == 4'528'350);
    CHECK(msg.levels[0].qty.units == 50'000'000);
}

TEST_CASE("the SBE decoder drives a feed end to end", "[venues][binance][sbe][feed]") {
    // The decoder satisfies the same contract as its JSON sibling, so the
    // whole recovery state machine comes for free — proven, not assumed.
    Feed<BinanceSbeDecoder, ArrayBook> feed("binance-sbe", btcusdt(), SequencePolicy::kBinanceSpot);

    const std::string snap =
        snapshot_frame(1, 100, -2, -8, {{4'528'350, 100'000'000}}, {{4'528'360, 100'000'000}});
    REQUIRE(feed.handle(snap) == FeedStatus::kApplied);
    REQUIRE(feed.synced());

    // Contiguous diff applies.
    REQUIRE(feed.handle(diff_frame(2, 98, 105, -2, -8, {{4'528'340, 25'000'000}}, {})) ==
            FeedStatus::kApplied);
    CHECK(feed.book().bids().size() == 2);

    // A gap stops the feed and keeps it stopped.
    CHECK(feed.handle(diff_frame(3, 110, 112, -2, -8, {{4'528'330, 10'000'000}}, {})) ==
          FeedStatus::kNeedsSnapshot);
    CHECK_FALSE(feed.synced());
    CHECK(feed.handle(diff_frame(4, 113, 114, -2, -8, {{4'528'320, 10'000'000}}, {})) ==
          FeedStatus::kNeedsSnapshot);
}
