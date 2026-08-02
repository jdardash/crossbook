// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// A structural JSON reader: allocation-free, forward-only, and deliberately
// small.
//
// WHY NOT A REAL JSON LIBRARY:
//
// 1. RAW TOKENS. Kraken's checksum is computed over price and quantity exactly
//    as they appear on the wire. A parser that hands back a double has already
//    destroyed the information needed to verify the book — see fixed.hpp. This
//    reader returns the untouched source span for every value, so the checksum
//    path sees the same bytes the exchange hashed.
//
// 2. ZERO DEPENDENCIES. crossbook is header-only and depends on nothing but the
//    standard library. Dragging in a JSON library to read six known field names
//    would be the single biggest obstacle to anyone adopting it.
//
// 3. KNOWN SHAPES. Venue messages are a fixed schema, not arbitrary documents.
//    Feed handlers routinely hand-roll this for exactly that reason.
//
// WHAT IT IS NOT: a general-purpose parser. It reads JSON of a known shape and
// rejects what it cannot navigate, rather than diagnosing why.
//
// WHAT IT DOES VALIDATE: structure, and the spelling of every scalar. A run of
// bytes between structural characters must be exactly `true`, `false`, `null`,
// or a number per RFC 8259 §6. Structure alone is not enough, and the gap was
// not theoretical: `{"checksum":12ab}` used to pass well_formed(), classify as
// a number on its first byte, and then fail to parse — and on the Kraken path a
// checksum that fails to parse is simply absent, so `has_checksum` stays false
// and verification silently stops running. A corrupted frame switched off the
// one mechanism that exists to notice corrupted frames. Same shape of bug for
// `{"a":truthy}` reading as kBool and `{"a":+5}` reading as a positive number
// that parse_fixed would have happily accepted as a price.
//
// It never reads out of bounds, never recurses without a depth limit, and never
// allocates — those are the properties that matter for something parsing
// untrusted bytes off a socket, and fuzz/fuzz_json.cpp exists to keep them true.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace crossbook {

enum class JsonType : std::uint8_t {
    kInvalid,
    kObject,
    kArray,
    kString,
    kNumber,
    kBool,
    kNull,
};

/// A value, as the untouched span of source that produced it.
struct JsonValue {
    std::string_view raw;  ///< For strings this INCLUDES the surrounding quotes.
    JsonType type{JsonType::kInvalid};

    [[nodiscard]] constexpr bool ok() const noexcept { return type != JsonType::kInvalid; }
    explicit constexpr operator bool() const noexcept { return ok(); }
};

namespace json {

/// Nesting depth beyond which input is rejected. A socket can deliver a million
/// open brackets; without this that is a stack overflow, which is a crash at
/// best and something worse at worst.
inline constexpr int kMaxDepth = 32;

constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// Advance past whitespace. Returns the new position.
constexpr std::size_t skip_space(std::string_view s, std::size_t pos) noexcept {
    while (pos < s.size() && is_space(s[pos])) {
        ++pos;
    }
    return pos;
}

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

/// Is `t` a JSON number, exactly as RFC 8259 §6 spells one?
///
///   -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?
///
/// Deliberately stricter than strtod in three ways that each correspond to a
/// way a corrupted or hostile frame could get a bad token past the scanner:
///
///   - No leading '+'. classify() used to answer kNumber for "+45283.5" and
///     parse_fixed() accepts the sign, so a plus-signed price would have been
///     read as a valid price rather than rejected as a malformed one.
///   - No leading zeros ("0x10", "012"), no bare ".5", no trailing "1.".
///   - No hex, no Infinity, no NaN — none of which a venue ever sends, and all
///     of which would decode to something other than what the exchange hashed.
constexpr bool is_json_number(std::string_view t) noexcept {
    std::size_t i = 0;
    const std::size_t n = t.size();
    if (i < n && t[i] == '-') {
        ++i;
    }
    if (i >= n) {
        return false;  // Sign with no digits.
    }
    if (t[i] == '0') {
        ++i;  // A leading zero must stand alone; "01" is not a JSON number.
    } else if (is_digit(t[i])) {
        while (i < n && is_digit(t[i])) {
            ++i;
        }
    } else {
        return false;
    }
    if (i < n && t[i] == '.') {
        ++i;
        const std::size_t frac_start = i;
        while (i < n && is_digit(t[i])) {
            ++i;
        }
        if (i == frac_start) {
            return false;  // "1." — a point with nothing after it.
        }
    }
    if (i < n && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < n && (t[i] == '+' || t[i] == '-')) {
            ++i;
        }
        const std::size_t exp_start = i;
        while (i < n && is_digit(t[i])) {
            ++i;
        }
        if (i == exp_start) {
            return false;  // "1e" — an exponent with no exponent.
        }
    }
    return i == n;  // Trailing junk ("12ab") is not a number.
}

/// Is `t` a complete JSON scalar: a literal, or a number?
///
/// This is what turns well_formed() from a structural check into the guarantee
/// its own docblock makes. Without it every one of `{"a":@@@}`,
/// `{"checksum":12ab}`, `{"a":truthy}`, `{"a":--1}`, `{"a":0x10}`, `{"a":.5}`
/// and `{"a":+5}` validated, and then classify() labelled them by first byte.
constexpr bool is_json_scalar(std::string_view t) noexcept {
    if (t == "true" || t == "false" || t == "null") {
        return true;
    }
    return is_json_number(t);
}

/// Skip a string literal starting at `pos` (which must index the opening
/// quote). Handles escapes, including an escaped backslash immediately before
/// the closing quote — the case a naive scan for the next '"' gets wrong.
constexpr std::size_t skip_string(std::string_view s, std::size_t pos) noexcept {
    if (pos >= s.size() || s[pos] != '"') {
        return std::string_view::npos;
    }
    ++pos;
    while (pos < s.size()) {
        const char c = s[pos];
        if (c == '\\') {
            pos += 2;  // Skip the escape and whatever it escapes.
            continue;
        }
        if (c == '"') {
            return pos + 1;
        }
        ++pos;
    }
    return std::string_view::npos;  // Unterminated.
}

/// Skip any JSON value starting at `pos`. Returns the position just past it, or
/// npos if the input is malformed or too deeply nested.
constexpr std::size_t skip_value(std::string_view s, std::size_t pos, int depth = 0) noexcept {
    if (depth > kMaxDepth) {
        return std::string_view::npos;
    }
    pos = skip_space(s, pos);
    if (pos >= s.size()) {
        return std::string_view::npos;
    }

    const char c = s[pos];
    if (c == '"') {
        return skip_string(s, pos);
    }

    if (c == '{' || c == '[') {
        const char close = (c == '{') ? '}' : ']';
        ++pos;
        pos = skip_space(s, pos);
        if (pos < s.size() && s[pos] == close) {
            return pos + 1;  // Empty.
        }
        while (pos < s.size()) {
            if (c == '{') {
                // Member: "key" : value
                pos = skip_space(s, pos);
                pos = skip_string(s, pos);
                if (pos == std::string_view::npos) {
                    return std::string_view::npos;
                }
                pos = skip_space(s, pos);
                if (pos >= s.size() || s[pos] != ':') {
                    return std::string_view::npos;
                }
                ++pos;
            }
            pos = skip_value(s, pos, depth + 1);
            if (pos == std::string_view::npos) {
                return std::string_view::npos;
            }
            pos = skip_space(s, pos);
            if (pos >= s.size()) {
                return std::string_view::npos;
            }
            if (s[pos] == close) {
                return pos + 1;
            }
            if (s[pos] != ',') {
                return std::string_view::npos;
            }
            ++pos;
        }
        return std::string_view::npos;
    }

    // Scalar: run to the next structural character, then insist that the run is
    // actually a JSON scalar. Accepting the run unconditionally — which is what
    // this did — is what let `{"checksum":12ab}` through well_formed().
    const std::size_t start = pos;
    while (pos < s.size()) {
        const char d = s[pos];
        if (d == ',' || d == '}' || d == ']' || is_space(d)) {
            break;
        }
        ++pos;
    }
    if (pos == start || !is_json_scalar(s.substr(start, pos - start))) {
        return std::string_view::npos;
    }
    return pos;
}

/// Classify a value by its first character.
///
/// Only meaningful for a span `skip_value` has already accepted — the first
/// byte tells you which arm of the grammar was taken, not that the arm was
/// spelled correctly. That is why skip_value now validates scalars: this
/// function is where a badly spelled scalar used to acquire a respectable type.
constexpr JsonType classify(std::string_view s, std::size_t pos) noexcept {
    pos = skip_space(s, pos);
    if (pos >= s.size()) {
        return JsonType::kInvalid;
    }
    switch (s[pos]) {
        case '{':
            return JsonType::kObject;
        case '[':
            return JsonType::kArray;
        case '"':
            return JsonType::kString;
        case 't':
        case 'f':
            return JsonType::kBool;
        case 'n':
            return JsonType::kNull;
        default:
            break;
    }
    // A leading '+' is NOT a JSON number, and treating it as one was live: the
    // token flows into parse_fixed, which does accept a sign, so "+45283.5"
    // would have become a perfectly ordinary price.
    const char c = s[pos];
    if (c == '-' || is_digit(c)) {
        return JsonType::kNumber;
    }
    return JsonType::kInvalid;
}

/// Is `s` a single, structurally complete JSON value with nothing trailing?
///
/// WHY CALLERS MUST RUN THIS FIRST:
///
/// `find` returns an invalid JsonValue both when a key is absent and when the
/// document is too malformed to navigate. Those look identical to a caller, and
/// conflating them is dangerous in exactly one direction: a truncated frame
/// whose `"b":[[` array never closes reads as "this message had no bids", so a
/// partial frame gets silently applied as an empty update and the book quietly
/// diverges from the venue.
///
/// Validating the whole span up front collapses that ambiguity — after this
/// returns true, an invalid `find` result means the key genuinely is not there.
///
/// That guarantee covers scalars too, and used not to. Structure-only
/// validation accepted `{"checksum":12ab}` and `{"a":truthy}`, so a caller that
/// had run this and been told "well-formed" could still be handed a kNumber
/// holding `12ab`. See `is_json_scalar`.
///
/// The cost is one extra structural pass over the frame. That is the same
/// trade simdjson makes with its two-stage design, and for a library whose
/// entire premise is provable correctness it is not a close call.
[[nodiscard]] constexpr bool well_formed(std::string_view s) noexcept {
    const std::size_t end = skip_value(s, 0);
    if (end == std::string_view::npos) {
        return false;
    }
    return skip_space(s, end) == s.size();
}

/// Look up a member of the object spanned by `object_raw`.
///
/// Compares the key literally, without unescaping: every field name in every
/// venue schema is plain ASCII, and accepting an escaped spelling of a key
/// would be a way to smuggle past the lookup rather than a feature.
///
/// DUPLICATE KEYS: first occurrence wins, and that is a deliberate choice, not
/// an accident of the loop shape. RFC 8259 leaves the behaviour undefined, and
/// the two available answers are not equally safe. Last-wins lets
/// `{"e":"depthUpdate","U":1,"u":2,...,"e":"trade"}` be routed as a depth
/// update by anything that reads the first `e` and as a trade by anything that
/// reads the last, which is a desync a peer can author at will. First-wins
/// makes every reader agree with every other reader on what the frame says.
/// `tests/test_json.cpp` pins this so a future rewrite of the scan cannot flip
/// it without a failing test.
[[nodiscard]] constexpr JsonValue find(std::string_view object_raw,
                                       std::string_view key) noexcept {
    std::size_t pos = skip_space(object_raw, 0);
    if (pos >= object_raw.size() || object_raw[pos] != '{') {
        return {};
    }
    ++pos;

    while (true) {
        pos = skip_space(object_raw, pos);
        if (pos >= object_raw.size()) {
            return {};
        }
        if (object_raw[pos] == '}') {
            return {};  // Exhausted; not found.
        }

        const std::size_t key_start = pos;
        const std::size_t key_end = skip_string(object_raw, pos);
        if (key_end == std::string_view::npos) {
            return {};
        }
        // Strip the surrounding quotes.
        const std::string_view found_key =
            object_raw.substr(key_start + 1, key_end - key_start - 2);

        pos = skip_space(object_raw, key_end);
        if (pos >= object_raw.size() || object_raw[pos] != ':') {
            return {};
        }
        ++pos;
        pos = skip_space(object_raw, pos);

        const std::size_t value_start = pos;
        const std::size_t value_end = skip_value(object_raw, pos);
        if (value_end == std::string_view::npos) {
            return {};
        }

        if (found_key == key) {
            return JsonValue{object_raw.substr(value_start, value_end - value_start),
                             classify(object_raw, value_start)};
        }

        pos = skip_space(object_raw, value_end);
        if (pos >= object_raw.size()) {
            return {};
        }
        if (object_raw[pos] == '}') {
            return {};
        }
        if (object_raw[pos] != ',') {
            return {};
        }
        ++pos;
    }
}

/// Invoke `fn(key, JsonValue)` for each member of the object spanned by
/// `object_raw`, in wire order, walking the object exactly once.
///
/// WHY THIS EXISTS WHEN `find` ALREADY DOES:
///
/// `find` restarts from the front of the object on every call, and a decoder
/// needs five or six fields per frame — the audit measured a Kraken frame
/// being re-walked 9.3x, and that re-walking was the majority of the ~2400 ns
/// decode cost. One member walk with the caller dispatching on key visits
/// every byte once. This is how production feed handlers read known-schema
/// wire formats, and it is the primitive both venue decoders are built on.
///
/// THE CONTRACT IS ALSO A VALIDATION PASS. Success — a return of true — means
/// the whole span was exactly one structurally valid JSON object with nothing
/// trailing, every nested value valid to full depth, every scalar spelled per
/// RFC 8259. That is precisely `well_formed`'s guarantee, which is why the
/// decoders no longer run `well_formed` first: the walk that reads the fields
/// IS the pass that used to be paid for separately. A caller that stops the
/// walk early (fn returns false) forfeits the guarantee for the unvisited
/// remainder, so the decoders never stop early on frames they intend to apply.
///
/// `key` is the raw span between the key's quotes, NOT unescaped. Every field
/// name in every venue schema is plain ASCII, and an escaped spelling of a
/// known key ("channel") is treated as an unknown key rather than
/// matched — same fail-closed reasoning as `find`, which this mirrors.
///
/// DUPLICATE KEYS: the walk reports every occurrence, in order. A caller that
/// captures only the first occurrence of each key reproduces `find`'s
/// first-wins semantics exactly; both decoders do, and tests/test_json.cpp
/// pins the equivalence so the two lookup paths cannot drift.
template <typename Fn>
[[nodiscard]] constexpr bool for_each_member(std::string_view object_raw, Fn&& fn) {
    std::size_t pos = skip_space(object_raw, 0);
    if (pos >= object_raw.size() || object_raw[pos] != '{') {
        return false;
    }
    ++pos;
    pos = skip_space(object_raw, pos);
    if (pos < object_raw.size() && object_raw[pos] == '}') {
        return skip_space(object_raw, pos + 1) == object_raw.size();  // Empty object.
    }

    while (true) {
        pos = skip_space(object_raw, pos);
        const std::size_t key_start = pos;
        const std::size_t key_end = skip_string(object_raw, pos);
        if (key_end == std::string_view::npos) {
            return false;
        }
        const std::string_view key =
            object_raw.substr(key_start + 1, key_end - key_start - 2);

        pos = skip_space(object_raw, key_end);
        if (pos >= object_raw.size() || object_raw[pos] != ':') {
            return false;
        }
        ++pos;
        pos = skip_space(object_raw, pos);

        const std::size_t value_start = pos;
        const std::size_t value_end = skip_value(object_raw, pos);
        if (value_end == std::string_view::npos) {
            return false;
        }

        if (!fn(key, JsonValue{object_raw.substr(value_start, value_end - value_start),
                               classify(object_raw, value_start)})) {
            return true;  // Caller stopped early; remainder unvalidated.
        }

        pos = skip_space(object_raw, value_end);
        if (pos >= object_raw.size()) {
            return false;
        }
        if (object_raw[pos] == '}') {
            return skip_space(object_raw, pos + 1) == object_raw.size();
        }
        if (object_raw[pos] != ',') {
            return false;
        }
        ++pos;
    }
}

/// Invoke `fn(JsonValue)` for each element of the array spanned by `array_raw`.
/// Stops early if `fn` returns false. Returns false on malformed input.
template <typename Fn>
[[nodiscard]] bool for_each(std::string_view array_raw, Fn&& fn) {
    std::size_t pos = skip_space(array_raw, 0);
    if (pos >= array_raw.size() || array_raw[pos] != '[') {
        return false;
    }
    ++pos;
    pos = skip_space(array_raw, pos);
    if (pos < array_raw.size() && array_raw[pos] == ']') {
        return true;  // Empty array is valid and common.
    }

    while (true) {
        pos = skip_space(array_raw, pos);
        const std::size_t value_start = pos;
        const std::size_t value_end = skip_value(array_raw, pos);
        if (value_end == std::string_view::npos) {
            return false;
        }
        if (!fn(JsonValue{array_raw.substr(value_start, value_end - value_start),
                          classify(array_raw, value_start)})) {
            return true;
        }
        pos = skip_space(array_raw, value_end);
        if (pos >= array_raw.size()) {
            return false;
        }
        if (array_raw[pos] == ']') {
            return true;
        }
        if (array_raw[pos] != ',') {
            return false;
        }
        ++pos;
    }
}

/// What `read_string` found.
enum class StringRead : std::uint8_t {
    /// Not a JSON string at all (or too short to have two quotes).
    kNotAString,
    /// A string whose contents contain no escapes. The returned body is the
    /// value, byte for byte.
    kPlain,
    /// A string whose contents contain at least one escape. The returned body
    /// is the RAW span, still escaped — it is NOT the value, and comparing it
    /// against a literal will be wrong. Use `string_equals`.
    kEscaped,
};

/// The contents of a string value, without its quotes, as a tri-state.
///
/// WHY THIS IS NOT JUST A string_view RETURN:
///
/// Unescaping cannot be done in place. `"BTC\/USD"` is eight bytes on the wire
/// and seven bytes as a value, and this scanner does not allocate, so there is
/// no span it could hand back that is the decoded string. The honest answer is
/// to say which case you are in and let the caller decide.
///
/// The alternative — returning an empty view for anything escaped — is what
/// this file used to do, and it is a silent wrong answer waiting for a caller.
/// `\/` is legal JSON that a venue is entitled to emit at any time, so
/// `{"symbol":"BTC\/USD"}` yielded an empty symbol. That was inert only while
/// nothing compared symbols; the moment a symbol comparison goes live it
/// becomes a false mismatch on a frame that was never malformed.
[[nodiscard]] constexpr StringRead read_string(const JsonValue& v,
                                               std::string_view& body) noexcept {
    body = {};
    if (v.type != JsonType::kString || v.raw.size() < 2) {
        return StringRead::kNotAString;
    }
    body = v.raw.substr(1, v.raw.size() - 2);
    for (char c : body) {
        if (c == '\\') {
            return StringRead::kEscaped;
        }
    }
    return StringRead::kPlain;
}

/// Does the string value `v` equal `expected`, decoding escapes as it goes?
///
/// The comparison callers actually want: it needs no buffer because it never
/// materialises the decoded string, only walks it. Handles the simple escapes
/// (`\" \\ \/ \b \f \n \r \t`).
///
/// `\uXXXX` and any undefined escape return false — fail-closed. A symbol or
/// channel name containing a unicode escape is not a name this library knows,
/// and answering "equal" on a string it cannot actually decode would be the
/// same class of mistake as returning empty.
[[nodiscard]] constexpr bool string_equals(const JsonValue& v,
                                           std::string_view expected) noexcept {
    std::string_view body;
    const StringRead status = read_string(v, body);
    if (status == StringRead::kNotAString) {
        return false;
    }
    if (status == StringRead::kPlain) {
        return body == expected;
    }

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < body.size()) {
        char decoded = '\0';
        if (body[i] != '\\') {
            decoded = body[i];
            ++i;
        } else {
            if (i + 1 >= body.size()) {
                return false;  // Trailing backslash; skip_string would not have
                               // produced this, but do not assume it.
            }
            const char escape = body[i + 1];
            i += 2;
            switch (escape) {
                case '"':
                    decoded = '"';
                    break;
                case '\\':
                    decoded = '\\';
                    break;
                case '/':
                    decoded = '/';
                    break;
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case 'n':
                    decoded = '\n';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                default:
                    return false;  // \u, or nonsense. Unsupported, not equal.
            }
        }
        if (j >= expected.size() || expected[j] != decoded) {
            return false;
        }
        ++j;
    }
    return j == expected.size();
}

/// The contents of a string value, without its quotes, for the plain case.
///
/// Convenience over `read_string` for the overwhelmingly common shape: a value
/// with no escapes in it. Returns an empty view for a non-string AND for an
/// escaped string, which is exactly the ambiguity `read_string` exists to
/// resolve — prefer `string_equals` when comparing, and `read_string` when the
/// difference between "absent" and "escaped" matters.
[[nodiscard]] constexpr std::string_view string_body(const JsonValue& v) noexcept {
    std::string_view body;
    return (read_string(v, body) == StringRead::kPlain) ? body : std::string_view{};
}

/// A numeric token as it appeared on the wire.
///
/// Accepts both a JSON number (Kraken) and a JSON string containing a number
/// (Binance) — the venues disagree, and the checksum needs the digits either
/// way.
[[nodiscard]] constexpr std::string_view number_token(const JsonValue& v) noexcept {
    if (v.type == JsonType::kNumber) {
        return v.raw;
    }
    if (v.type == JsonType::kString) {
        return string_body(v);
    }
    return {};
}

/// Parse an unsigned integer from a raw token. Returns false on overflow or any
/// non-digit, rather than saturating: a sequence number that silently wrapped
/// would break gap detection in the least visible way possible.
[[nodiscard]] constexpr bool parse_u64(std::string_view token, std::uint64_t& out) noexcept {
    if (token.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (0xFFFFFFFFFFFFFFFFULL - digit) / 10ULL) {
            return false;
        }
        value = value * 10ULL + digit;
    }
    out = value;
    return true;
}

}  // namespace json
}  // namespace crossbook
