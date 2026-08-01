// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "crossbook/json.hpp"

using namespace crossbook;

TEST_CASE("find locates scalar members", "[json]") {
    constexpr std::string_view doc = R"({"a":1,"b":"two","c":true,"d":null,"e":-3.5})";
    CHECK(json::find(doc, "a").raw == "1");
    CHECK(json::find(doc, "a").type == JsonType::kNumber);
    CHECK(json::find(doc, "b").raw == "\"two\"");
    CHECK(json::find(doc, "b").type == JsonType::kString);
    CHECK(json::find(doc, "c").type == JsonType::kBool);
    CHECK(json::find(doc, "d").type == JsonType::kNull);
    CHECK(json::find(doc, "e").raw == "-3.5");
    CHECK_FALSE(json::find(doc, "missing"));
}

TEST_CASE("find skips nested structures to reach later keys", "[json]") {
    // The case a naive scanner gets wrong: a nested object containing the key
    // being searched for must not shadow the top-level one.
    constexpr std::string_view doc =
        R"({"nested":{"target":"wrong","deep":[1,2,{"target":"also wrong"}]},"target":"right"})";
    CHECK(json::string_body(json::find(doc, "target")) == "right");
}

TEST_CASE("find handles whitespace everywhere", "[json]") {
    constexpr std::string_view doc = "{ \n \"a\" : \t 1 , \r\n \"b\" : [ 1 , 2 ] }";
    CHECK(json::find(doc, "a").raw == "1");
    CHECK(json::find(doc, "b").type == JsonType::kArray);
}

TEST_CASE("string skipping respects escapes", "[json]") {
    // A backslash-escaped quote must not terminate the string, and an escaped
    // backslash immediately before the closing quote must not swallow it.
    constexpr std::string_view doc = R"({"a":"has \" quote","b":"ends with backslash \\","c":7})";
    CHECK(json::find(doc, "c").raw == "7");
    CHECK(json::find(doc, "a").type == JsonType::kString);
    CHECK(json::find(doc, "b").type == JsonType::kString);
}

TEST_CASE("string_body strips quotes and rejects escapes", "[json]") {
    constexpr std::string_view doc = R"({"plain":"BTC/USD","escaped":"a\/b"})";
    CHECK(json::string_body(json::find(doc, "plain")) == "BTC/USD");
    // Escaped content is refused rather than returned raw and misinterpreted.
    CHECK(json::string_body(json::find(doc, "escaped")).empty());
}

TEST_CASE("for_each walks array elements", "[json]") {
    constexpr std::string_view doc = R"({"xs":[1,22,333]})";
    std::string joined;
    const bool ok = json::for_each(json::find(doc, "xs").raw, [&](const JsonValue& v) {
        joined += v.raw;
        joined += ',';
        return true;
    });
    CHECK(ok);
    CHECK(joined == "1,22,333,");
}

TEST_CASE("for_each handles empty and nested arrays", "[json]") {
    CHECK(json::for_each("[]", [](const JsonValue&) { return true; }));

    constexpr std::string_view doc = R"({"xs":[["a","b"],["c","d"]]})";
    int pairs = 0;
    const bool ok = json::for_each(json::find(doc, "xs").raw, [&](const JsonValue& v) {
        CHECK(v.type == JsonType::kArray);
        ++pairs;
        return true;
    });
    CHECK(ok);
    CHECK(pairs == 2);
}

TEST_CASE("for_each stops early when asked", "[json]") {
    constexpr std::string_view doc = R"({"xs":[1,2,3,4,5]})";
    int seen = 0;
    const bool ok = json::for_each(json::find(doc, "xs").raw, [&](const JsonValue&) {
        ++seen;
        return seen < 2;
    });
    CHECK(ok);
    CHECK(seen == 2);
}

TEST_CASE("number_token accepts both venue conventions", "[json]") {
    // Kraken sends numbers; Binance sends numeric strings. The checksum needs
    // the digits either way.
    constexpr std::string_view doc = R"({"kraken":45285.2,"binance":"45285.2"})";
    CHECK(json::number_token(json::find(doc, "kraken")) == "45285.2");
    CHECK(json::number_token(json::find(doc, "binance")) == "45285.2");
}

TEST_CASE("parse_u64 rejects rather than saturating", "[json]") {
    std::uint64_t v = 0;
    CHECK(json::parse_u64("0", v));
    CHECK(v == 0);
    CHECK(json::parse_u64("18446744073709551615", v));
    CHECK(v == 18446744073709551615ULL);

    // A sequence number that silently wrapped would break gap detection in the
    // least visible way available.
    CHECK_FALSE(json::parse_u64("18446744073709551616", v));
    CHECK_FALSE(json::parse_u64("99999999999999999999999", v));
    CHECK_FALSE(json::parse_u64("", v));
    CHECK_FALSE(json::parse_u64("-1", v));
    CHECK_FALSE(json::parse_u64("1.5", v));
    CHECK_FALSE(json::parse_u64("abc", v));
}

TEST_CASE("malformed input is rejected, never read past the end", "[json]") {
    // Every one of these arrives off a socket eventually.
    for (std::string_view bad : {
             "{",
             "}",
             "{\"a\"",
             "{\"a\":",
             "{\"a\":}",
             "{\"a\" 1}",
             "{\"unterminated",
             "{\"a\":\"unterminated}",
             "{\"a\":[1,2}",
             "{\"a\":[1,,2]}",
             "[1,2,3]",
             "",
             "   ",
             "null",
         }) {
        INFO("input=" << bad);
        CHECK_FALSE(json::find(bad, "a").ok());
    }
}

TEST_CASE("deep nesting is rejected rather than overflowing the stack", "[json]") {
    // A socket can deliver a million open brackets. Without a depth limit that
    // is a stack overflow, which is a crash at best.
    std::string deep = "{\"a\":";
    for (int i = 0; i < 500; ++i) {
        deep += '[';
    }
    for (int i = 0; i < 500; ++i) {
        deep += ']';
    }
    deep += '}';
    CHECK_FALSE(json::find(deep, "a").ok());
}

TEST_CASE("scanner is constexpr-evaluable", "[json]") {
    // Which proves it cannot allocate or touch global state.
    static_assert(json::find(R"({"a":1,"b":2})", "b").raw == std::string_view("2"));
    SUCCEED();
}

TEST_CASE("well_formed distinguishes truncation from an absent key", "[json]") {
    // The distinction this function exists for. A truncated frame whose bid
    // array never closes must not read as "this message had no bids", because
    // that decodes as a valid empty update and gets applied to the book.
    CHECK(json::well_formed(R"({"a":1,"b":[[1,2]]})"));
    CHECK(json::well_formed(R"({})"));
    CHECK(json::well_formed(R"([1,2,3])"));
    CHECK(json::well_formed("  {\"a\":1}  "));

    CHECK_FALSE(json::well_formed(R"({"e":"depthUpdate","U":1,"u":2,"b":[[)"));
    CHECK_FALSE(json::well_formed(R"({"a":1)"));
    CHECK_FALSE(json::well_formed(R"({"a":1} trailing)"));
    CHECK_FALSE(json::well_formed(R"({"a":"unterminated)"));
    CHECK_FALSE(json::well_formed(""));

    // Both a well-formed object missing the key and a truncated one return an
    // invalid find(); only well_formed() tells them apart.
    CHECK_FALSE(json::find(R"({"a":1})", "b").ok());
    CHECK(json::well_formed(R"({"a":1})"));
}
