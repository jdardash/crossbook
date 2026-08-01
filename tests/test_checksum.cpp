// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

TEST_CASE("CRC32 matches the standard known-answer vector", "[checksum]") {
    // The check value every CRC-32/ISO-HDLC implementation is defined against.
    CHECK(crc32("123456789") == 0xCBF43926U);
    CHECK(crc32("") == 0x00000000U);
    CHECK(crc32("a") == 0xE8B7BE43U);
    CHECK(crc32("abc") == 0x352441C2U);
}

TEST_CASE("Crc32 streams identically to a single update", "[checksum]") {
    // The verifier feeds the payload level by level and never materialises the
    // concatenated string, so streaming has to be exactly equivalent.
    Crc32 streamed;
    streamed.update("123");
    streamed.update("456");
    streamed.update("789");
    CHECK(streamed.value() == crc32("123456789"));
}

TEST_CASE("Crc32 resets to a clean state", "[checksum]") {
    Crc32 c;
    c.update("garbage");
    c.reset();
    c.update("123456789");
    CHECK(c.value() == 0xCBF43926U);
}

namespace {

/// A book with `n` levels per side, priced around 45285.2 at Kraken's BTC/USD
/// scales, so the checksum payload resembles a real one.
template <typename BookT>
BookT make_book(std::size_t n) {
    BookT book(InstrumentSpec{"BTC/USD", 1, 8});
    for (std::size_t i = 0; i < n; ++i) {
        const auto step = static_cast<std::int64_t>(i);
        book.apply(Side::kAsk, Price{452853 + step}, Qty{100000 + step});
        book.apply(Side::kBid, Price{452852 - step}, Qty{200000 + step});
    }
    return book;
}

}  // namespace

TEST_CASE("checksum payload is asks low-to-high then bids high-to-low", "[checksum]") {
    auto book = make_book<MapBook>(2);

    // Asks ascend from 452853; bids descend from 452852.
    const std::string expected =
        std::string("452853") + "100000" +  // best ask
        "452854" + "100001" +               // next ask
        "452852" + "200000" +               // best bid
        "452851" + "200001";                // next bid

    CHECK(kraken_checksum_payload(book) == expected);
    CHECK(kraken_checksum(book) == crc32(expected));
}

TEST_CASE("checksum covers at most ten levels per side", "[checksum]") {
    // Kraken checksums the top 10 "regardless of subscription depth", so a
    // deeper book must produce the same checksum as one truncated to 10.
    auto deep = make_book<MapBook>(25);
    auto shallow = make_book<MapBook>(10);

    CHECK(kraken_checksum_payload(deep) == kraken_checksum_payload(shallow));
    CHECK(kraken_checksum(deep) == kraken_checksum(shallow));

    // 10 levels per side, each contributing a 6-digit price and a 6-digit qty.
    CHECK(kraken_checksum_payload(deep).size() == 10 * 2 * (6 + 6));
}

TEST_CASE("checksum tolerates a book shallower than ten levels", "[checksum]") {
    auto book = make_book<MapBook>(3);
    CHECK(kraken_checksum_payload(book).size() == 3 * 2 * (6 + 6));
    CHECK(kraken_checksum(book) == crc32(kraken_checksum_payload(book)));
}

TEST_CASE("checksum is empty-book safe", "[checksum]") {
    MapBook book(InstrumentSpec{"BTC/USD", 1, 8});
    CHECK(kraken_checksum_payload(book).empty());
    CHECK(kraken_checksum(book) == crc32(""));
}

TEST_CASE("both book implementations produce the same checksum", "[checksum][book]") {
    // The fast book is only useful if it is indistinguishable from the
    // reference. Checksum equality is the strongest single-value statement of
    // that, since it depends on ordering, depth, and every mantissa.
    for (std::size_t n : {1U, 2U, 5U, 10U, 11U, 50U}) {
        INFO("levels per side = " << n);
        CHECK(kraken_checksum(make_book<MapBook>(n)) == kraken_checksum(make_book<ArrayBook>(n)));
    }
}

TEST_CASE("checksum changes when any level changes", "[checksum]") {
    // A checksum that ignores part of the book would verify nothing. Perturb
    // each component in turn and require the value to move.
    auto base = make_book<MapBook>(10);
    const std::uint32_t base_sum = kraken_checksum(base);

    SECTION("quantity change at the touch") {
        auto b = make_book<MapBook>(10);
        b.apply(Side::kAsk, Price{452853}, Qty{999999});
        CHECK(kraken_checksum(b) != base_sum);
    }
    SECTION("new level inside the top ten") {
        auto b = make_book<MapBook>(10);
        b.apply(Side::kBid, Price{452852 + 1}, Qty{111111});
        CHECK(kraken_checksum(b) != base_sum);
    }
    SECTION("level removed") {
        auto b = make_book<MapBook>(10);
        b.apply(Side::kAsk, Price{452853}, Qty{0});
        CHECK(kraken_checksum(b) != base_sum);
    }
    SECTION("change beyond the tenth level does not") {
        auto b = make_book<MapBook>(10);
        b.apply(Side::kAsk, Price{452853 + 40}, Qty{123456});
        CHECK(kraken_checksum(b) == base_sum);
    }
}

// NOTE ON END-TO-END VALIDATION
//
// These tests pin down the two halves independently: CRC32 against its standard
// check vector, and the payload construction against Kraken's documented
// transformation and ordering. What they cannot do is prove the composition
// matches a real exchange message, because that needs a real exchange message.
//
// That is the job of the live verifier and the committed capture fixtures (the
// v0.1 gate is a 100% match rate over a 24h Kraken capture, with every
// divergence enumerated). Nothing here should be read as a substitute for it.
