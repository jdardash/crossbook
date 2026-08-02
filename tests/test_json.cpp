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

TEST_CASE("read_string separates absent from escaped", "[json]") {
    // string_body returns an empty view for both "not a string" and "escaped",
    // and a caller cannot tell those apart from the view alone. That ambiguity
    // is what made the escaped-symbol defect invisible: {"symbol":"BTC\/USD"}
    // is legal JSON that yielded an empty symbol, which reads as "no symbol"
    // rather than "a symbol this reader declined to decode".
    constexpr std::string_view doc = R"({"plain":"BTC/USD","escaped":"BTC\/USD","num":7})";

    std::string_view body;
    CHECK(json::read_string(json::find(doc, "plain"), body) == json::StringRead::kPlain);
    CHECK(body == "BTC/USD");

    CHECK(json::read_string(json::find(doc, "escaped"), body) == json::StringRead::kEscaped);
    CHECK(body == R"(BTC\/USD)");  // Raw, still escaped, and explicitly labelled so.

    CHECK(json::read_string(json::find(doc, "num"), body) == json::StringRead::kNotAString);
    CHECK(body.empty());

    CHECK(json::read_string(json::find(doc, "missing"), body) == json::StringRead::kNotAString);
}

TEST_CASE("string_equals decodes escapes so a legal frame is not a false mismatch", "[json]") {
    // \/ is a valid JSON escape for '/' and a venue may emit it at any time.
    // Comparing the raw body against "BTC/USD" fails; comparing the decoded
    // value succeeds. Once symbol comparison is live, the difference between
    // those two is a spurious resync on a frame that was never wrong.
    constexpr std::string_view doc =
        R"({"a":"BTC\/USD","b":"BTC/USD","c":"line\nbreak","d":"quote\"inside","e":"back\\slash"})";
    CHECK(json::string_equals(json::find(doc, "a"), "BTC/USD"));
    CHECK(json::string_equals(json::find(doc, "b"), "BTC/USD"));
    CHECK(json::string_equals(json::find(doc, "c"), "line\nbreak"));
    CHECK(json::string_equals(json::find(doc, "d"), "quote\"inside"));
    CHECK(json::string_equals(json::find(doc, "e"), "back\\slash"));

    // Still a comparison, so it must also say no when the answer is no.
    CHECK_FALSE(json::string_equals(json::find(doc, "a"), "ETH/USD"));
    CHECK_FALSE(json::string_equals(json::find(doc, "a"), "BTC/USD "));
    CHECK_FALSE(json::string_equals(json::find(doc, "a"), "BTC/US"));

    // \u is not decoded. Fail closed rather than claim a match this reader
    // cannot actually verify. (The escape below spells "ABC" in real JSON.)
    constexpr std::string_view uni = "{\"s\":\"\\u00" "41BC\"}";
    CHECK_FALSE(json::string_equals(json::find(uni, "s"), "ABC"));
    std::string_view uni_body;
    CHECK(json::read_string(json::find(uni, "s"), uni_body) == json::StringRead::kEscaped);

    // A non-string never equals anything.
    CHECK_FALSE(json::string_equals(json::find(R"({"n":42})", "n"), "42"));
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

TEST_CASE("well_formed validates scalars, not only structure", "[json]") {
    // Every one of these was structurally navigable and therefore validated,
    // which broke the guarantee well_formed's own docblock makes: a caller that
    // had run it could still be handed a value whose type was a lie.
    for (std::string_view bad : {
             R"({"a":@@@})",
             R"({"checksum":12ab})",
             R"({"a":truthy})",
             R"({"a":--1})",
             R"({"a":0x10})",
             R"({"a":.5})",
             R"({"a":+5})",
             R"({"a":tru})",
             R"({"a":nulll})",
             R"({"a":falsey})",
             R"({"a":01})",
             R"({"a":1.})",
             R"({"a":1e})",
             R"({"a":1e+})",
             R"({"a":-})",
             R"({"a":Infinity})",
             R"({"a":NaN})",
             R"([1,@,3])",
         }) {
        INFO("input=" << bad);
        CHECK_FALSE(json::well_formed(bad));
        // And the key must not be reachable either, or the rejection is
        // cosmetic: callers reach values through find(), not well_formed().
        CHECK_FALSE(json::find(bad, "a").ok());
        CHECK_FALSE(json::find(bad, "checksum").ok());
    }
}

TEST_CASE("well_formed still accepts every number a venue actually sends", "[json]") {
    // The strictness above is only defensible if it does not reject real wire
    // data. Kraken prices, Binance quantities, exponent notation, negatives.
    for (std::string_view good : {
             R"({"a":0})",
             R"({"a":-0})",
             R"({"a":45285.2})",
             R"({"a":0.00100000})",
             R"({"a":-3.5})",
             R"({"a":1e5})",
             R"({"a":1E-10})",
             R"({"a":1.5e+3})",
             R"({"a":2418130093})",
             R"({"a":true,"b":false,"c":null})",
         }) {
        INFO("input=" << good);
        CHECK(json::well_formed(good));
        CHECK(json::find(good, "a").ok());
    }
}

TEST_CASE("a corrupted checksum is rejected, not silently downgraded", "[json]") {
    // The mechanism this whole fix exists for. A Kraken frame whose checksum
    // field is corrupted used to pass structural validation, classify as
    // kNumber, fail parse_u64, and leave has_checksum false — so the frame was
    // applied to the book with verification quietly switched off. The corrupted
    // frame turned off the one thing that would have caught it.
    constexpr std::string_view corrupt = R"({"channel":"book","checksum":12ab})";
    CHECK_FALSE(json::well_formed(corrupt));
    CHECK_FALSE(json::find(corrupt, "checksum").ok());

    // Not a number by first byte either, now that the run must spell one.
    constexpr std::string_view plus = R"({"price":+45283.5})";
    CHECK_FALSE(json::well_formed(plus));
    CHECK_FALSE(json::find(plus, "price").ok());
    // parse_fixed accepts a leading sign, so classify() answering kNumber here
    // was enough on its own to turn "+45283.5" into an ordinary price.
    CHECK(json::classify("+45283.5", 0) == JsonType::kInvalid);
    CHECK(json::classify("-45283.5", 0) == JsonType::kNumber);
}

TEST_CASE("duplicate keys resolve first-wins, deliberately", "[json]") {
    // RFC 8259 leaves this undefined and the two answers are not equally safe.
    // Pinned so a rewrite of the scan loop cannot flip it silently.
    constexpr std::string_view doc = R"({"a":1,"b":2,"a":999})";
    CHECK(json::find(doc, "a").raw == "1");

    // The reason it matters: last-wins would let one frame be routed as a depth
    // update by a reader that takes the first "e" and as a trade by a reader
    // that takes the last. First-wins makes every reader agree.
    constexpr std::string_view desync =
        R"({"e":"depthUpdate","U":1,"u":2,"b":[],"a":[],"e":"trade"})";
    CHECK(json::string_body(json::find(desync, "e")) == "depthUpdate");

    // Also true when the shadowing value is a different shape entirely, which
    // is the version that would change a value's TYPE under a reader's feet.
    constexpr std::string_view retyped = R"({"checksum":2418130093,"checksum":"nope"})";
    CHECK(json::find(retyped, "checksum").type == JsonType::kNumber);
    CHECK(json::find(retyped, "checksum").raw == "2418130093");
}

TEST_CASE("for_each_member visits what find answers, in one walk", "[json]") {
    // The decoders read five or six fields per frame. Through find() each
    // lookup restarts from the front; for_each_member visits every member
    // once and the caller dispatches on key. The two must agree exactly, or
    // the decoder rewrite changed semantics rather than cost.
    constexpr std::string_view doc =
        R"({"channel":"book","type":"update","data":[{"px":1}],"n":42})";

    JsonValue channel;
    JsonValue type;
    JsonValue data;
    JsonValue n;
    std::size_t visited = 0;
    const bool ok = json::for_each_member(doc, [&](std::string_view key, const JsonValue& v) {
        ++visited;
        if (key == "channel" && !channel.ok()) {
            channel = v;
        } else if (key == "type" && !type.ok()) {
            type = v;
        } else if (key == "data" && !data.ok()) {
            data = v;
        } else if (key == "n" && !n.ok()) {
            n = v;
        }
        return true;
    });
    REQUIRE(ok);
    CHECK(visited == 4);
    CHECK(channel.raw == json::find(doc, "channel").raw);
    CHECK(type.raw == json::find(doc, "type").raw);
    CHECK(data.raw == json::find(doc, "data").raw);
    CHECK(data.type == JsonType::kArray);
    CHECK(n.raw == json::find(doc, "n").raw);

    // First-wins duplicates: capture-if-empty reproduces find() exactly.
    constexpr std::string_view dup = R"({"e":"depthUpdate","e":"trade"})";
    JsonValue e;
    REQUIRE(json::for_each_member(dup, [&](std::string_view key, const JsonValue& v) {
        if (key == "e" && !e.ok()) {
            e = v;
        }
        return true;
    }));
    CHECK(e.raw == json::find(dup, "e").raw);
}

TEST_CASE("a completed for_each_member walk is a well_formed guarantee", "[json]") {
    // The decoders dropped their well_formed() pre-pass on the strength of
    // this contract: success means the whole span was one valid object with
    // nothing trailing and every nested value valid. So every rejection
    // well_formed makes, the walk must make too.
    const auto walks = [](std::string_view s) {
        return json::for_each_member(s, [](std::string_view, const JsonValue&) { return true; });
    };

    // Truncations, at every depth.
    CHECK_FALSE(walks(R"({"b":[[)"));
    CHECK_FALSE(walks(R"({"a":{"b":1})"));
    CHECK_FALSE(walks(R"({"a":1)"));

    // Bad scalars nested inside values the walk merely skips over. This is
    // the {"checksum":12ab} class the docblock on well_formed records.
    CHECK_FALSE(walks(R"({"checksum":12ab})"));
    CHECK_FALSE(walks(R"({"data":[{"qty":truthy}]})"));
    CHECK_FALSE(walks(R"({"px":+5})"));

    // Trailing bytes after the object.
    CHECK_FALSE(walks(R"({"a":1} )" "x"));
    CHECK_FALSE(walks(R"({"a":1}{"b":2})"));

    // Not an object at all: the walk refuses, and the decoders route these
    // through well_formed() to keep valid-but-not-ours frames kIgnored.
    CHECK_FALSE(walks(R"([1,2,3])"));
    CHECK_FALSE(walks(R"("just a string")"));

    // And the shapes venues actually send still walk.
    CHECK(walks(R"({})"));
    CHECK(walks(R"({"channel":"heartbeat"})"));
    CHECK(walks(
        R"({"e":"depthUpdate","E":1571889248277,"s":"BTCUSDT","U":157,"u":160,)"
        R"("b":[["0.0024","10"]],"a":[["0.0026","100"]]})"));
}
