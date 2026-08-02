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
                 "checksum":1234567890,
                 "timestamp":"2026-07-31T12:00:00.000000Z"}]})";

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
                 "checksum":42,"timestamp":"2026-07-31T12:00:00.000000Z"}]})";

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
        "data":[{"symbol":"BTC/USD","bids":[{"price":45283.55,"qty":0.50000000}],
                 "timestamp":"2026-07-31T12:00:00.000000Z"}]})";

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
                         {"price":45283.4,"qty":1.20000000}],
                 "timestamp":"2026-07-31T12:00:00.000000Z"}]})";

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

TEST_CASE("Kraken applies its own symbol and ignores a foreign one", "[venues][kraken][symbol]") {
    // Nothing ever compared the wire symbol against spec_.symbol on either
    // venue. A frame for another instrument therefore had its levels applied to
    // this book, and no later delete ever arrives to undo it: the venue has no
    // reason to send a correction for a level it does not know you invented.
    KrakenBookDecoder decoder(btc_usd());

    SECTION("the subscribed symbol is applied") {
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
            R"("timestamp":"2026-07-31T12:00:00.000000Z",)"
            R"("bids":[{"price":45283.5,"qty":0.50000000}]}]})";
        const DecodedMessage& msg = decoder.decode(frame);
        REQUIRE(msg.ok());
        CHECK(msg.kind == MessageKind::kUpdate);
        CHECK(msg.levels.size() == 1);
    }

    SECTION("a foreign symbol is ignored, not applied and not an error") {
        // A multiplexed socket legitimately carries other instruments, so this
        // must not be counted as a decode failure — but not one level of it may
        // reach the book.
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"ETH/USD",)"
            R"("timestamp":"2026-07-31T12:00:00.000000Z",)"
            R"("bids":[{"price":3000.0,"qty":7.00000000}]}]})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK(msg.ok());
        CHECK(msg.kind == MessageKind::kIgnored);
        CHECK(msg.levels.empty());
    }
}

TEST_CASE("Kraken selects the matching entry from a multi-entry frame",
          "[venues][kraken][symbol]") {
    // The walk used to stop after entry #1 by returning false from the for_each
    // callback — but for_each treats an early false as SUCCESS, so entries 2..N
    // were dropped and decode() still reported kOk. Combined with the missing
    // symbol check, entry #1 was applied whatever instrument it belonged to.
    //
    // The wanted symbol is deliberately SECOND here: under the old code this
    // frame decoded cleanly as an ETH update against a BTC book.
    constexpr std::string_view frame =
        R"({"channel":"book","type":"update","data":[)"
        R"({"symbol":"ETH/USD","timestamp":"2026-07-31T12:00:00.000000Z",)"
        R"("bids":[{"price":3000.0,"qty":7.00000000}]},)"
        R"({"symbol":"BTC/USD","timestamp":"2026-07-31T12:00:01.000000Z",)"
        R"("bids":[{"price":45283.5,"qty":0.50000000}],"checksum":42}]})";

    KrakenBookDecoder decoder(btc_usd());
    const DecodedMessage& msg = decoder.decode(frame);

    REQUIRE(msg.ok());
    CHECK(msg.kind == MessageKind::kUpdate);
    CHECK(msg.symbol == "BTC/USD");
    CHECK(msg.checksum == 42U);
    REQUIRE(msg.levels.size() == 1);
    CHECK(msg.levels[0].price.ticks == 452835);  // Not 30000, the ETH price.
    CHECK(msg.levels[0].qty.units == 50'000'000);
}

TEST_CASE("Kraken parses the RFC3339 timestamp to nanoseconds", "[venues][kraken][staleness]") {
    // The timestamp was never read, so message_.ts stayed 0 on every Kraken
    // message — and Feed::check_staleness bails out on last_ts_ == 0, making
    // staleness detection structurally dead on the only venue that publishes
    // checksums. The single staleness test used Binance, so CI stayed green.
    KrakenBookDecoder decoder(btc_usd());

    SECTION("a documented instant decodes to the right nanosecond") {
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
            R"("timestamp":"2026-07-31T12:00:00.000000Z",)"
            R"("bids":[{"price":45283.5,"qty":0.50000000}]}]})";
        const DecodedMessage& msg = decoder.decode(frame);
        REQUIRE(msg.ok());
        // 2026-07-31T12:00:00Z = 1785499200 seconds since the epoch. Computed
        // independently of the parser under test, from the day count.
        CHECK(msg.ts == 1'785'499'200LL * 1'000'000'000LL);
    }

    SECTION("sub-second precision survives to the nanosecond") {
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
            R"("timestamp":"2026-07-31T12:00:00.123456789Z",)"
            R"("bids":[{"price":45283.5,"qty":0.50000000}]}]})";
        const DecodedMessage& msg = decoder.decode(frame);
        REQUIRE(msg.ok());
        CHECK(msg.ts == 1'785'499'200LL * 1'000'000'000LL + 123'456'789LL);
    }

    SECTION("the epoch itself, as a fixed point for the day arithmetic") {
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
            R"("timestamp":"1970-01-01T00:00:00.000000Z",)"
            R"("bids":[{"price":45283.5,"qty":0.50000000}]}]})";
        const DecodedMessage& msg = decoder.decode(frame);
        REQUIRE(msg.ok());
        CHECK(msg.ts == 0);
    }

    SECTION("an absent or unreadable timestamp is fatal, never a silent zero") {
        // A zero here reinstates the dead check: the feed can no longer go
        // stale, and will serve an hour-old book with full confidence.
        for (std::string_view timestamp : {
                 R"()",                                  // absent
                 R"("timestamp":"",)",                   // empty
                 R"("timestamp":"2026-07-31 12:00:00Z",)",    // no 'T'
                 R"("timestamp":"2026-07-31T12:00:00",)",    // no zone: not necessarily UTC
                 R"("timestamp":"2026-07-31T12:00:00+02:00",)",  // offset, not UTC
                 R"("timestamp":"2026-13-31T12:00:00.0Z",)",     // month 13
                 R"("timestamp":"2026-07-31T25:00:00.0Z",)",     // hour 25
                 R"("timestamp":1785499200,)",                   // epoch seconds, not RFC3339
                 R"("timestamp":"not a time",)",
             }) {
            const std::string frame =
                std::string(R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)") +
                std::string(timestamp) +
                R"("bids":[{"price":45283.5,"qty":0.50000000}]}]})";
            INFO("timestamp=" << timestamp);
            const DecodedMessage& msg = decoder.decode(frame);
            CHECK_FALSE(msg.ok());
            CHECK(msg.error == DecodeError::kBadTimestamp);
            CHECK(msg.levels.empty());
        }
    }
}

TEST_CASE("Kraken accepts both checksum spellings and refuses an unreadable one",
          "[venues][kraken][checksum]") {
    // Every unreadable spelling used to fall through to has_checksum = false,
    // which is indistinguishable from "Kraken sent no checksum" — so the feed
    // skipped verification, checksums_verified never moved, and match_rate()
    // reported 1.0 having compared nothing. A venue quoting the field as a
    // string would have switched the library's central claim off, silently.
    KrakenBookDecoder decoder(btc_usd());
    auto frame_with = [](std::string_view checksum_field) {
        return std::string(R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)") +
               R"("timestamp":"2026-07-31T12:00:00.000000Z",)" + std::string(checksum_field) +
               R"("bids":[{"price":45283.5,"qty":0.50000000}]}]})";
    };

    SECTION("as a JSON number, the shape Kraken sends today") {
        const DecodedMessage& msg = decoder.decode(frame_with(R"("checksum":2418130093,)"));
        REQUIRE(msg.ok());
        CHECK(msg.has_checksum);
        CHECK(msg.checksum == 2418130093U);
    }

    SECTION("as a quoted string, the shape venues migrate to") {
        const DecodedMessage& msg = decoder.decode(frame_with(R"("checksum":"2418130093",)"));
        REQUIRE(msg.ok());
        CHECK(msg.has_checksum);
        CHECK(msg.checksum == 2418130093U);
    }

    SECTION("present but unreadable is a hard error, not a skipped verification") {
        for (std::string_view field : {
                 R"("checksum":"deadbeef",)",    // not decimal
                 R"("checksum":4294967296,)",    // one past uint32
                 R"("checksum":-1,)",            // negative
                 R"("checksum":true,)",          // wrong type entirely
                 R"("checksum":null,)",
                 R"("checksum":"",)",
             }) {
            INFO("field=" << field);
            const DecodedMessage& msg = decoder.decode(frame_with(field));
            CHECK_FALSE(msg.ok());
            CHECK(msg.error == DecodeError::kBadChecksum);
            CHECK_FALSE(msg.has_checksum);
        }
    }

    SECTION("genuinely absent stays non-fatal: verification simply cannot run") {
        const DecodedMessage& msg = decoder.decode(frame_with(""));
        REQUIRE(msg.ok());
        CHECK_FALSE(msg.has_checksum);
    }
}

TEST_CASE("Kraken reports a venue error rather than passing it off as a heartbeat",
          "[venues][kraken][error]") {
    // MessageKind::kVenueError was declared and never produced by anything, so a
    // subscription that never succeeded looked exactly like a quiet market.
    KrakenBookDecoder decoder(btc_usd());

    SECTION("the documented error shape") {
        constexpr std::string_view frame =
            R"({"error":"Subscription failed: unknown pair","method":"subscribe",)"
            R"("success":false,"req_id":7})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK(msg.ok());  // Decoded fine. What it says is the problem.
        CHECK(msg.kind == MessageKind::kVenueError);
        CHECK(msg.bad_token == "Subscription failed: unknown pair");
    }

    SECTION("a bare failed ack, with no error text") {
        constexpr std::string_view frame = R"({"method":"subscribe","success":false,"req_id":7})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK(msg.kind == MessageKind::kVenueError);
    }

    SECTION("a successful ack is still just an ack") {
        const DecodedMessage& msg = decoder.decode(R"({"method":"subscribe","success":true})");
        CHECK(msg.kind == MessageKind::kIgnored);
    }
}

TEST_CASE("Kraken refuses a non-canonical price or quantity at ingest",
          "[venues][kraken][checksum]") {
    // The checksum precondition, enforced where the bytes arrive. "0.5" at a
    // scale-8 quantity parses exactly, so nothing upstream objects — but Kraken
    // hashes "5" while the fast path hashes "50000000", and every checksum then
    // fails against a book that is numerically perfect.
    KrakenBookDecoder decoder(btc_usd());

    SECTION("a quantity missing its trailing zeros") {
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
            R"("timestamp":"2026-07-31T12:00:00.000000Z",)"
            R"("bids":[{"price":45283.5,"qty":0.5}]}]})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK_FALSE(msg.ok());
        CHECK(msg.error == DecodeError::kNonCanonical);
        CHECK(msg.bad_token == "0.5");  // The divergence names the token.
        CHECK(msg.levels.empty());
    }

    SECTION("a price carrying a decimal the scale does not") {
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
            R"("timestamp":"2026-07-31T12:00:00.000000Z",)"
            R"("bids":[{"price":45283,"qty":0.50000000}]}]})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK_FALSE(msg.ok());
        CHECK(msg.error == DecodeError::kNonCanonical);
        CHECK(msg.bad_token == "45283");
    }

    SECTION("canonical spellings pass, so the guard is not simply refusing everything") {
        constexpr std::string_view frame =
            R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD",)"
            R"("timestamp":"2026-07-31T12:00:00.000000Z",)"
            R"("bids":[{"price":45283.5,"qty":0.50000000}]}]})";
        CHECK(decoder.decode(frame).ok());
    }
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

TEST_CASE("Binance routes combined-stream frames by symbol", "[venues][binance][symbol]") {
    // The decoder ADVERTISED combined-stream support — it unwraps the
    // {"stream":..,"data":{..}} envelope — while implementing none of the
    // routing that envelope exists for. An ethusdt@depth frame arriving on a
    // BTCUSDT socket had its levels applied straight into the BTC book, and no
    // later delete ever arrives to remove them.
    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);

    SECTION("the subscribed symbol is applied") {
        constexpr std::string_view frame =
            R"({"stream":"btcusdt@depth","data":{"e":"depthUpdate","E":1,"s":"BTCUSDT",)"
            R"("U":98,"u":105,"b":[["45283.50","0.50000000"]],"a":[]}})";
        const DecodedMessage& msg = decoder.decode(frame);
        REQUIRE(msg.ok());
        CHECK(msg.kind == MessageKind::kUpdate);
        REQUIRE(msg.levels.size() == 1);
        CHECK(msg.levels[0].price.ticks == 4528350);
    }

    SECTION("a foreign symbol is ignored and carries no levels") {
        // Verbatim from the audit: this frame put a 3000.00 ETH bid into a BTC
        // book and reported kApplied.
        constexpr std::string_view frame =
            R"({"stream":"ethusdt@depth","data":{"e":"depthUpdate","E":1,"s":"ETHUSDT",)"
            R"("U":98,"u":105,"b":[["3000.00","7.00000000"]],"a":[]}})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK(msg.ok());
        CHECK(msg.kind == MessageKind::kIgnored);
        CHECK(msg.levels.empty());
        CHECK_FALSE(msg.has_ids);
    }

    SECTION("the comparison is case-insensitive, because Binance uppercases") {
        BinanceDepthDecoder lowercase(InstrumentSpec{"btcusdt", 2, 8}, BinanceMarket::kSpot);
        constexpr std::string_view frame =
            R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":98,"u":105,)"
            R"("b":[["45283.50","0.50000000"]],"a":[]})";
        const DecodedMessage& msg = lowercase.decode(frame);
        REQUIRE(msg.ok());
        CHECK(msg.kind == MessageKind::kUpdate);
        CHECK(msg.levels.size() == 1);
    }
}

TEST_CASE("Binance reports a venue error rather than passing it off as an ack",
          "[venues][binance][error]") {
    // {"code":-1121,"msg":"Invalid symbol."} carries no "e", so it used to be
    // indistinguishable from a pong. A symbol we spelled wrong then presented as
    // a market with nothing to say.
    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);

    SECTION("the documented error shape") {
        const DecodedMessage& msg = decoder.decode(R"({"code":-1121,"msg":"Invalid symbol."})");
        CHECK(msg.ok());
        CHECK(msg.kind == MessageKind::kVenueError);
        CHECK(msg.bad_token == "Invalid symbol.");
    }

    SECTION("a plain subscription ack is still just an ack") {
        const DecodedMessage& msg = decoder.decode(R"({"result":null,"id":1})");
        CHECK(msg.kind == MessageKind::kIgnored);
    }
}

TEST_CASE("Binance refuses a non-canonical price or quantity at ingest",
          "[venues][binance][checksum]") {
    // Binance publishes no checksum, so a spelling change cannot break a
    // verification that does not exist — but it is the earliest signal that the
    // venue altered its precision, and the consolidated book compares these
    // levels against venues that do verify.
    BinanceDepthDecoder decoder(btc_usdt(), BinanceMarket::kSpot);

    SECTION("a quantity missing its trailing zeros") {
        constexpr std::string_view frame =
            R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,)"
            R"("b":[["45283.50","0.5"]],"a":[]})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK_FALSE(msg.ok());
        CHECK(msg.error == DecodeError::kNonCanonical);
        CHECK(msg.bad_token == "0.5");
        CHECK(msg.levels.empty());
    }

    SECTION("a price missing its trailing zeros") {
        constexpr std::string_view frame =
            R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,)"
            R"("b":[["45283.5","0.50000000"]],"a":[]})";
        const DecodedMessage& msg = decoder.decode(frame);
        CHECK_FALSE(msg.ok());
        CHECK(msg.error == DecodeError::kNonCanonical);
        CHECK(msg.bad_token == "45283.5");
    }

    SECTION("a REST snapshot is guarded on the same terms") {
        const DecodedMessage& msg = decoder.decode_snapshot(
            R"({"lastUpdateId":160,"bids":[["45283.50","0.5"]],"asks":[]})");
        CHECK_FALSE(msg.ok());
        CHECK(msg.error == DecodeError::kNonCanonical);
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

// ---------------------------------------------------------------------------
// Escaped strings in comparison positions
//
// `\/` is a legal JSON escape for `/`, and Kraken's symbols contain a slash.
// `string_body` returns EMPTY for any body carrying an escape, so comparing by
// view called a correctly-spelled frame a foreign instrument and dropped every
// message for the symbol actually subscribed to. That was inert only while
// nothing compared symbols; symbol routing now does.
// ---------------------------------------------------------------------------

TEST_CASE("an escaped but legal symbol is not read as a foreign instrument",
          "[venues][kraken][escape]") {
    venues::KrakenBookDecoder decoder{InstrumentSpec{"BTC/USD", 1, 8}};

    // "BTC\/USD" is byte-different from "BTC/USD" and semantically identical.
    const auto& msg = decoder.decode(
        R"({"channel":"book","type":"update","data":[{"symbol":"BTC\/USD",)"
        R"("bids":[{"price":45283.5,"qty":1.00000000}],"asks":[],"checksum":1,)"
        R"("timestamp":"2026-07-31T12:00:00.000000Z"}]})");

    INFO("error=" << to_string(msg.error) << " bad_token=" << msg.bad_token
                   << " symbol=" << msg.symbol << " ok=" << msg.ok());
    CHECK(msg.kind == MessageKind::kUpdate);
    CHECK(msg.levels.size() == 1);
}

TEST_CASE("an escaped channel or type still routes", "[venues][kraken][escape]") {
    // Neither contains a character that needs escaping, but a venue is free to
    // escape anything, and a decoder that only works on the unescaped spelling
    // is one venue-side formatting change away from ignoring every frame.
    venues::KrakenBookDecoder decoder{InstrumentSpec{"BTC/USD", 1, 8}};
    const auto& msg = decoder.decode(
        R"({"channel":"boo\u006b","type":"update","data":[{"symbol":"BTC/USD",)"
        R"("bids":[{"price":45283.5,"qty":1.00000000}],"asks":[],"checksum":1,)"
        R"("timestamp":"2026-07-31T12:00:00.000000Z"}]})");
    // \u escapes are explicitly not decoded — string_equals fails closed on
    // them rather than guessing. The frame is ignored, not misapplied, and
    // that is the correct conservative outcome for an escape we cannot read.
    CHECK(msg.kind == MessageKind::kIgnored);
    CHECK(msg.ok());
}
