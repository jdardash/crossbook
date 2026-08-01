// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"
#include "crossbook/venues/binance.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;
using namespace crossbook::venues;

// ---------------------------------------------------------------------------
// Kraken
// ---------------------------------------------------------------------------

namespace {
InstrumentSpec btc_usd() { return InstrumentSpec{"BTC/USD", 1, 8}; }
}  // namespace

TEST_CASE("Kraken snapshot decodes both sides", "[venues][kraken]") {
    constexpr std::string_view frame = R"({
        "channel":"book","type":"snapshot",
        "data":[{"symbol":"BTC/USD",
                 "bids":[{"price":45283.5,"qty":0.50000000},
                         {"price":45283.4,"qty":1.20000000}],
                 "asks":[{"price":45283.6,"qty":0.30000000}],
                 "checksum":1234567890}]})";

    KrakenBookDecoder decoder(btc_usd());
    const DecodedMessage& msg = decoder.decode(frame);

    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kSnapshot);
    CHECK(msg.symbol == "BTC/USD");
    CHECK(msg.has_checksum);
    CHECK(msg.checksum == 1234567890U);
    REQUIRE(msg.levels.size() == 3);

    CHECK(msg.levels[0].side == Side::kBid);
    CHECK(msg.levels[0].price.ticks == 452835);   // 45283.5 at scale 1
    CHECK(msg.levels[0].qty.units == 50'000'000);  // 0.5 at scale 8
    CHECK(msg.levels[2].side == Side::kAsk);
    CHECK(msg.levels[2].price.ticks == 452836);
}

TEST_CASE("Kraken update with a zero quantity removes a level", "[venues][kraken]") {
    constexpr std::string_view frame = R"({
        "channel":"book","type":"update",
        "data":[{"symbol":"BTC/USD","bids":[{"price":45283.5,"qty":0.00000000}],
                 "checksum":42}]})";

    KrakenBookDecoder decoder(btc_usd());
    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kUpdate);
    REQUIRE(msg.levels.size() == 1);
    CHECK(msg.levels[0].qty.units == 0);
}

TEST_CASE("Kraken non-book frames are ignored, not rejected", "[venues][kraken]") {
    KrakenBookDecoder decoder(btc_usd());
    for (std::string_view frame : {
             R"({"channel":"heartbeat"})",
             R"({"channel":"status","data":[{"system":"online"}]})",
             R"({"method":"subscribe","success":true})",
             R"({"channel":"trade","type":"update","data":[]})",
         }) {
        INFO("frame=" << frame);
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK(msg.ok());
        CHECK(msg.kind == MessageKind::kIgnored);
    }
}

TEST_CASE("Kraken decoder reports precision loss with the offending token",
          "[venues][kraken]") {
    // The venue changing tick size is a real event. Silently rounding would
    // produce a book that fails every checksum for no discoverable reason.
    constexpr std::string_view frame = R"({
        "channel":"book","type":"update",
        "data":[{"symbol":"BTC/USD","bids":[{"price":45283.55,"qty":0.5}]}]})";

    KrakenBookDecoder decoder(btc_usd());  // price_scale is 1
    const DecodedMessage& msg = decoder.decode(frame);
    CHECK_FALSE(msg.ok());
    CHECK(msg.error == DecodeError::kPrecisionLoss);
    CHECK(msg.bad_token == "45283.55");
}

TEST_CASE("Kraken malformed frames are rejected without crashing", "[venues][kraken]") {
    KrakenBookDecoder decoder(btc_usd());
    for (std::string_view frame : {
             R"({"channel":"book","type":"update"})",
             R"({"channel":"book","type":"update","data":"not an array"})",
             R"({"channel":"book","type":"update","data":[1,2,3]})",
             R"({"channel":"book","type":"update","data":[{"bids":"nope"}]})",
             R"({"channel":"book","type":"update","data":[{"bids":[{"price":1}]}]})",
             R"({"channel":"book",)",
         }) {
        INFO("frame=" << frame);
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK((msg.kind == MessageKind::kIgnored || !msg.ok()));
    }
}

TEST_CASE("a decoded Kraken snapshot reproduces the exchange checksum end to end",
          "[venues][kraken][checksum]") {
    // The full chain: JSON on the wire -> raw tokens -> exact mantissas ->
    // book -> CRC32. This is the property the whole library is built around,
    // exercised through every layer at once.
    //
    // The expected value is computed from Kraken's documented algorithm applied
    // to this synthetic book, not taken from a live message — see the note in
    // test_checksum.cpp about what only a live capture can prove.
    constexpr std::string_view frame = R"({
        "channel":"book","type":"snapshot",
        "data":[{"symbol":"BTC/USD",
                 "asks":[{"price":45283.6,"qty":0.30000000},
                         {"price":45283.7,"qty":1.00000000}],
                 "bids":[{"price":45283.5,"qty":0.50000000},
                         {"price":45283.4,"qty":1.20000000}]}]})";

    KrakenBookDecoder decoder(btc_usd());
    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());

    ArrayBook book(btc_usd());
    for (const LevelUpdate& lvl : msg.levels) {
        book.apply(lvl.side, lvl.price, lvl.qty);
    }

    // Asks low-to-high then bids high-to-low, each level contributing the
    // mantissa digits of price then quantity.
    const std::string expected_payload =
        std::string("452836") + "30000000" +
        "452837" + "100000000" +
        "452835" + "50000000" +
        "452834" + "120000000";

    CHECK(kraken_checksum_payload(book) == expected_payload);
    CHECK(kraken_checksum(book) == crc32(expected_payload));
}

// ---------------------------------------------------------------------------
// Binance
// ---------------------------------------------------------------------------

namespace {
InstrumentSpec btc_usdt() { return InstrumentSpec{"BTCUSDT", 2, 8}; }
}  // namespace

TEST_CASE("Binance spot depth update decodes", "[venues][binance]") {
    constexpr std::string_view frame = R"({
        "e":"depthUpdate","E":1571889248277,"s":"BTCUSDT",
        "U":157,"u":160,
        "b":[["45283.50","0.50000000"],["45283.40","0.00000000"]],
        "a":[["45283.60","0.30000000"]]})";

    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);
    const DecodedMessage& msg = decoder.decode(frame);

    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kUpdate);
    CHECK(msg.symbol == "BTCUSDT");
    CHECK(msg.has_ids);
    CHECK(msg.ids.first_id == 157);
    CHECK(msg.ids.final_id == 160);
    CHECK(msg.ts == 1571889248277LL * 1'000'000);
    CHECK_FALSE(msg.has_checksum);  // Binance publishes none.

    REQUIRE(msg.levels.size() == 3);
    CHECK(msg.levels[0].side == Side::kBid);
    CHECK(msg.levels[0].price.ticks == 4528350);  // 45283.50 at scale 2
    CHECK(msg.levels[1].qty.units == 0);          // Deletion.
    CHECK(msg.levels[2].side == Side::kAsk);
}

TEST_CASE("Binance combined-stream wrapper is unwrapped", "[venues][binance]") {
    constexpr std::string_view frame = R"({
        "stream":"btcusdt@depth",
        "data":{"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,
                "b":[["100.00","1.00000000"]],"a":[]}})";

    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);
    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kUpdate);
    REQUIRE(msg.levels.size() == 1);
    CHECK(msg.levels[0].price.ticks == 10000);
}

TEST_CASE("Binance futures requires pu and spot does not", "[venues][binance]") {
    constexpr std::string_view without_pu = R"({
        "e":"depthUpdate","E":1,"s":"BTCUSDT","U":157,"u":160,
        "b":[["100.00","1.00000000"]]})";

    SECTION("spot accepts it") {
        BinanceDepthDecoder spot(btc_usdt(), BinanceMarket::kSpot);
        CHECK(spot.decode(without_pu).ok());
        CHECK(spot.policy() == SequencePolicy::kBinanceSpot);
    }
    SECTION("futures rejects it rather than silently losing gap detection") {
        // Degrading to "no continuity checking" without saying so is how a
        // futures book drifts away from the venue undetected.
        BinanceDepthDecoder futures(btc_usdt(), BinanceMarket::kFutures);
        const DecodedMessage& msg = futures.decode(without_pu);
        CHECK_FALSE(msg.ok());
        CHECK(msg.error == DecodeError::kBadSequence);
        CHECK(futures.policy() == SequencePolicy::kBinanceFutures);
    }
}

TEST_CASE("Binance futures update with pu decodes", "[venues][binance]") {
    constexpr std::string_view frame = R"({
        "e":"depthUpdate","E":1,"T":2,"s":"BTCUSDT","U":157,"u":160,"pu":149,
        "b":[["100.00","1.00000000"]],"a":[]})";

    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kFutures);
    const DecodedMessage& msg = decoder.decode(frame);
    REQUIRE(msg.ok());
    CHECK(msg.ids.prev_id == 149);
    CHECK(msg.ids.final_id == 160);
}

TEST_CASE("Binance REST snapshot decodes", "[venues][binance]") {
    constexpr std::string_view body = R"({
        "lastUpdateId":160,
        "bids":[["45283.50","0.50000000"]],
        "asks":[["45283.60","0.30000000"]]})";

    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);
    const DecodedMessage& msg = decoder.decode_snapshot(body);
    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kSnapshot);
    CHECK(msg.ids.final_id == 160);
    REQUIRE(msg.levels.size() == 2);
}

TEST_CASE("Binance non-depth frames are ignored", "[venues][binance]") {
    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);
    for (std::string_view frame : {
             R"({"result":null,"id":1})",
             R"({"e":"trade","E":1,"s":"BTCUSDT","p":"100.00"})",
             R"({"e":"kline","E":1})",
         }) {
        INFO("frame=" << frame);
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK(msg.ok());
        CHECK(msg.kind == MessageKind::kIgnored);
    }
}

TEST_CASE("Binance malformed frames are rejected without crashing", "[venues][binance]") {
    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);
    for (std::string_view frame : {
             R"({"e":"depthUpdate","E":1,"s":"X"})",
             R"({"e":"depthUpdate","U":"not a number","u":2})",
             R"({"e":"depthUpdate","U":1,"u":2,"b":"nope"})",
             R"({"e":"depthUpdate","U":1,"u":2,"b":[["only-one-field"]]})",
             R"({"e":"depthUpdate","U":1,"u":2,"b":[[")",
         }) {
        INFO("frame=" << frame);
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK((msg.kind == MessageKind::kIgnored || !msg.ok()));
    }
}

TEST_CASE("decoders reuse their level buffer across calls", "[venues]") {
    // Steady-state decoding must not allocate; the reserve happens once at
    // construction. test_no_alloc.cpp enforces this for the book, and the same
    // discipline applies here.
    constexpr std::string_view frame = R"({
        "e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,
        "b":[["100.00","1.00000000"]],"a":[]})";

    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);
    for (int i = 0; i < 100; ++i) {
        const DecodedMessage& msg = decoder.decode(frame);
        REQUIRE(msg.ok());
        REQUIRE(msg.levels.size() == 1);  // reset() clears without freeing.
    }
}
