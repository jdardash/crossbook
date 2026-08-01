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
// WHAT IT IS NOT: a validating parser. It reads well-formed JSON of a known
// shape and rejects what it cannot navigate, rather than diagnosing why. It
// never reads out of bounds, never recurses without a depth limit, and never
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

    // Scalar: run to the next structural character.
    const std::size_t start = pos;
    while (pos < s.size()) {
        const char d = s[pos];
        if (d == ',' || d == '}' || d == ']' || is_space(d)) {
            break;
        }
        ++pos;
    }
    return (pos > start) ? pos : std::string_view::npos;
}

/// Classify a value by its first character.
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
    const char c = s[pos];
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
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

/// The contents of a string value, without its quotes.
///
/// Returns the raw span: no unescaping. Callers use this for symbols and
/// numeric-strings, none of which are ever escaped in practice. A value
/// containing a backslash is rejected rather than silently mishandled.
[[nodiscard]] constexpr std::string_view string_body(const JsonValue& v) noexcept {
    if (v.type != JsonType::kString || v.raw.size() < 2) {
        return {};
    }
    const std::string_view body = v.raw.substr(1, v.raw.size() - 2);
    for (char c : body) {
        if (c == '\\') {
            return {};
        }
    }
    return body;
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
