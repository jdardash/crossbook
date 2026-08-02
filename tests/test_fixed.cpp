// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "crossbook/fixed.hpp"

using namespace crossbook;

TEST_CASE("parse_fixed handles plain decimals exactly", "[fixed]") {
    CHECK(parse_fixed("0", 0).mantissa == 0);
    CHECK(parse_fixed("1", 0).mantissa == 1);
    CHECK(parse_fixed("1", 2).mantissa == 100);
    CHECK(parse_fixed("1.5", 2).mantissa == 150);
    CHECK(parse_fixed("0.01", 2).mantissa == 1);
    CHECK(parse_fixed("-1.5", 2).mantissa == -150);
    CHECK(parse_fixed("+1.5", 2).mantissa == 150);
    CHECK(parse_fixed("123.456", 3).mantissa == 123456);
}

TEST_CASE("parse_fixed is exact where a double would not be", "[fixed]") {
    // 0.1 + 0.2 != 0.3 in binary floating point. Two books fed these values
    // must nonetheless agree on whether a price level exists, which is the
    // entire reason prices are stored as integer mantissas.
    const auto a = parse_fixed("0.1", 8);
    const auto b = parse_fixed("0.2", 8);
    const auto c = parse_fixed("0.3", 8);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(c.ok());
    CHECK(a.mantissa + b.mantissa == c.mantissa);
}

TEST_CASE("parse_fixed refuses to round", "[fixed]") {
    // A venue quoting more precision than the instrument's configured scale is
    // a real event (they change tick sizes). Silently rounding would produce a
    // book that is subtly wrong and passes every internal check while failing
    // every exchange checksum. Refusing surfaces it immediately.
    CHECK(parse_fixed("1.005", 2).error == ParseError::kPrecisionLoss);
    CHECK(parse_fixed("0.000001", 2).error == ParseError::kPrecisionLoss);

    // Trailing zeros beyond the scale are not precision loss: they carry no
    // information and the value is still exactly representable.
    CHECK(parse_fixed("1.500", 2).ok());
    CHECK(parse_fixed("1.500", 2).mantissa == 150);
}

TEST_CASE("parse_fixed rejects malformed input", "[fixed]") {
    CHECK(parse_fixed("", 2).error == ParseError::kEmpty);
    CHECK(parse_fixed("abc", 2).error == ParseError::kMalformed);
    CHECK(parse_fixed("1.2.3", 2).error == ParseError::kMalformed);
    CHECK(parse_fixed("1.2x", 2).error == ParseError::kMalformed);
    CHECK(parse_fixed("-", 2).error == ParseError::kMalformed);
    CHECK(parse_fixed(".", 2).error == ParseError::kMalformed);
    CHECK(parse_fixed("1e", 2).error == ParseError::kMalformed);
    CHECK(parse_fixed("1e+", 2).error == ParseError::kMalformed);
    CHECK(parse_fixed("1", kMaxScale + 1).error == ParseError::kScaleTooLarge);
}

TEST_CASE("parse_fixed accepts exponent notation", "[fixed]") {
    // Venues quote plain decimals, but JSON permits exponents and the fuzzer
    // will absolutely find them.
    CHECK(parse_fixed("1e2", 0).mantissa == 100);
    CHECK(parse_fixed("1.5e2", 0).mantissa == 150);
    CHECK(parse_fixed("1.5E2", 0).mantissa == 150);
    CHECK(parse_fixed("15e-1", 1).mantissa == 15);
    CHECK(parse_fixed("1e-3", 3).mantissa == 1);
    CHECK(parse_fixed("1e-3", 2).error == ParseError::kPrecisionLoss);
}

TEST_CASE("parse_fixed detects overflow rather than wrapping", "[fixed]") {
    CHECK(parse_fixed("99999999999999999999", 0).error == ParseError::kOverflow);
    CHECK(parse_fixed("9223372036854775808", 0).error == ParseError::kOverflow);
    CHECK(parse_fixed("9223372036854775807", 0).mantissa == 9223372036854775807LL);
    CHECK(parse_fixed("1", 18).mantissa == 1000000000000000000LL);
    CHECK(parse_fixed("10", 18).error == ParseError::kOverflow);
}

TEST_CASE("parse_fixed is usable at compile time", "[fixed]") {
    // constexpr-evaluable means the parser cannot allocate, cannot touch a
    // locale, and cannot reach for anything platform-dependent.
    static_assert(parse_fixed("45285.2", 1).mantissa == 452852);
    static_assert(parse_fixed("1.005", 2).error == ParseError::kPrecisionLoss);
    SUCCEED();
}

TEST_CASE("format_fixed round-trips", "[fixed]") {
    CHECK(format_fixed(452852, 1) == "45285.2");
    CHECK(format_fixed(100000, 8) == "0.00100000");
    CHECK(format_fixed(1, 8) == "0.00000001");
    CHECK(format_fixed(0, 8) == "0.00000000");
    CHECK(format_fixed(0, 0) == "0");
    CHECK(format_fixed(-150, 2) == "-1.50");
    CHECK(format_fixed(150, 0) == "150");
}

TEST_CASE("is_canonical_at_scale validates wire spelling against a scale", "[fixed]") {
    CHECK(is_canonical_at_scale("45285.2", 1));
    CHECK(is_canonical_at_scale("0.00100000", 8));
    CHECK_FALSE(is_canonical_at_scale("1.005", 2));
    CHECK_FALSE(is_canonical_at_scale("garbage", 2));
}

TEST_CASE("is_canonical_at_scale catches missing trailing zeros", "[fixed][checksum]") {
    // THE CASE THE PREDICATE EXISTS FOR, and the one its predecessor could not
    // see. `round_trips` parsed, re-formatted, re-parsed and compared MANTISSAS
    // — an identity for anything that parses at all — so it returned true for
    // every input below and could only ever restate what parse_fixed had already
    // said. Missing trailing zeros are exactly what breaks Kraken's checksum
    // identity, and they parse perfectly.
    CHECK_FALSE(is_canonical_at_scale("0.5", 8));       // wire "5" vs mantissa "50000000"
    CHECK_FALSE(is_canonical_at_scale("45285.20", 1));  // one decimal too many, still exact
    CHECK_FALSE(is_canonical_at_scale("1", 2));         // no fraction where the scale needs two
    CHECK_FALSE(is_canonical_at_scale("1.00", 0));      // a fraction where the scale allows none
    CHECK_FALSE(is_canonical_at_scale("01.50", 2));     // leading zero: not what format_fixed emits
    CHECK_FALSE(is_canonical_at_scale("1.5e1", 1));     // exponent form parses; is not canonical

    // All of the above parse exactly. That is precisely why a numeric
    // comparison cannot distinguish them from the canonical spelling.
    CHECK(parse_fixed("0.5", 8).ok());
    CHECK(parse_fixed("45285.20", 1).ok());
    CHECK(parse_fixed("1", 2).ok());
    CHECK(parse_fixed("1.00", 0).ok());
    CHECK(parse_fixed("01.50", 2).ok());
    CHECK(parse_fixed("1.5e1", 1).ok());

    // And the canonical spellings of the same values are accepted, so the
    // predicate is not simply rejecting everything.
    CHECK(is_canonical_at_scale("0.50000000", 8));
    CHECK(is_canonical_at_scale("45285.2", 1));
    CHECK(is_canonical_at_scale("1.00", 2));
    CHECK(is_canonical_at_scale("1", 0));
    CHECK(is_canonical_at_scale("15.0", 1));

    // A leading '+' carries no information and JSON permits it.
    CHECK(is_canonical_at_scale("+1.50", 2));
    CHECK(is_canonical_at_scale("-1.50", 2));
}

TEST_CASE("is_canonical_at_scale agrees with format_fixed by construction", "[fixed]") {
    // The predicate and the formatter must not be able to drift apart: whatever
    // format_fixed emits for a mantissa is the definition of canonical for it.
    for (std::int64_t mantissa : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1},
                                  std::int64_t{150}, std::int64_t{-150}, std::int64_t{100000},
                                  std::int64_t{9223372036854775807LL}}) {
        for (Scale scale : {Scale{0}, Scale{1}, Scale{2}, Scale{8}, Scale{18}}) {
            const std::string canonical = format_fixed(mantissa, scale);
            INFO("mantissa=" << mantissa << " scale=" << static_cast<int>(scale) << " -> "
                             << canonical);
            // Scale 18 overflows the reparse for large mantissas; skip those,
            // since an unrepresentable value is not a spelling question.
            if (!parse_fixed(canonical, scale).ok()) {
                continue;
            }
            CHECK(is_canonical_at_scale(canonical, scale));
        }
    }
}

// -------------------------------------------------------------------------
// The load-bearing property: Kraken's checksum token is the mantissa's digits.
// -------------------------------------------------------------------------

TEST_CASE("checksum_token matches Kraken's documented transformation", "[fixed][checksum]") {
    // From docs.kraken.com/api/docs/guides/spot-ws-book-v2:
    //   price 45285.2     -> "452852"
    //   qty   0.00100000  -> "100000"
    //
    // Kraken specifies this as "remove the decimal point, then strip leading
    // zeros". Applied to a value already stored as a fixed-point mantissa at
    // the instrument's scale, that is just the mantissa in decimal. This test
    // is what pins that equivalence down; if it ever fails, the fast path in
    // checksum.hpp is invalid and must go back to string formatting.

    auto kraken_transform = [](std::string_view decimal) {
        std::string s;
        for (char c : decimal) {
            if (c != '.') {
                s.push_back(c);
            }
        }
        const std::size_t first = s.find_first_not_of('0');
        return first == std::string::npos ? std::string("0") : s.substr(first);
    };

    struct Case {
        std::string_view decimal;
        Scale scale;
    };
    // Every case is spelled canonically at its declared scale, which is the
    // precondition the identity depends on. See the test below.
    const Case cases[] = {
        {"45285.2", 1},     // The documented price example.
        {"0.00100000", 8},  // The documented quantity example.
        {"1.0", 1},
        {"0.1", 1},
        {"0.00000001", 8},
        {"12345.67890", 5},
        {"999999.9", 1},
        {"0.50000000", 8},
    };

    for (const Case& c : cases) {
        INFO("decimal=" << c.decimal << " scale=" << static_cast<int>(c.scale));
        REQUIRE(is_canonical_at_scale(c.decimal, c.scale));  // Precondition holds.
        const ParseResult parsed = parse_fixed(c.decimal, c.scale);
        REQUIRE(parsed.ok());
        CHECK(checksum_token(parsed.mantissa) == kraken_transform(c.decimal));
    }
}

TEST_CASE("the checksum-token identity requires canonical wire spelling",
          "[fixed][checksum]") {
    // THE ONE ASSUMPTION THE FAST CHECKSUM PATH RESTS ON.
    //
    // Kraken computes its checksum from the value "as it appears in the
    // message". Taking the mantissa's digits instead is only equivalent when
    // the venue spells the value at exactly the instrument's scale, trailing
    // zeros included.
    //
    // Kraken does: its own example quantity is "0.00100000", padded to the
    // 8-decimal quantity precision. But if a venue ever sent "0.5" for a
    // scale-8 quantity, the wire transformation yields "5" while the mantissa
    // yields "50000000", and every checksum would fail with a book that is
    // numerically perfect.
    //
    // This test pins the boundary so the assumption is visible rather than
    // buried, and `is_canonical_at_scale()` is what detects a violation at
    // ingest — both decoders call it on every level they read.

    auto wire_transform = [](std::string_view decimal) {
        std::string s;
        for (char c : decimal) {
            if (c != '.') {
                s.push_back(c);
            }
        }
        const std::size_t first = s.find_first_not_of('0');
        return first == std::string::npos ? std::string("0") : s.substr(first);
    };

    SECTION("canonical spelling: identity holds") {
        REQUIRE(is_canonical_at_scale("0.50000000", 8));
        CHECK(checksum_token(parse_fixed("0.50000000", 8).mantissa) ==
              wire_transform("0.50000000"));
    }

    SECTION("non-canonical spelling: identity breaks, and the guard flags it") {
        // "0.5" is a perfectly valid number and parses exactly; what it is not
        // is the canonical scale-8 spelling, so it must not be trusted for a
        // checksum.
        const ParseResult parsed = parse_fixed("0.5", 8);
        REQUIRE(parsed.ok());
        CHECK(checksum_token(parsed.mantissa) == "50000000");
        CHECK(wire_transform("0.5") == "5");
        CHECK(checksum_token(parsed.mantissa) != wire_transform("0.5"));

        // THE GUARD ITSELF. This section previously asserted only that
        // format_fixed produced something different from the wire text, and
        // never called the predicate named in its own title — so the predicate
        // could be, and was, incapable of returning false here.
        CHECK_FALSE(is_canonical_at_scale("0.5", 8));
        CHECK(format_fixed(parsed.mantissa, 8) != std::string("0.5"));
    }
}
