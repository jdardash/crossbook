// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Fuzz the JSON scanner directly.
//
// WHY THIS FILE EXISTS SEPARATELY FROM fuzz_decode.cpp:
//
// json.hpp's header comment has always claimed that "fuzz/fuzz_json.cpp exists
// to keep them true" of its safety properties. That file did not exist. The
// scanner was reached only indirectly, through fuzz_decode's exercise_scanner,
// which looks up fifteen hardcoded key names and otherwise lets the venue
// decoders decide which corners get visited. A decoder that rejects a frame
// early is a decoder that stops the fuzzer from ever reaching the scanner code
// underneath it, so the most-quoted invariant in the library was being kept
// true by a citation rather than by a target.
//
// A hand-written scanner fed untrusted exchange bytes is exactly the thing that
// deserves its own target, so here it is, with oracles rather than a bare
// "did not crash":
//
//   1. MEMORY SAFETY. Every span the scanner hands back lies inside the input.
//      ASAN covers reads past the end; these checks cover the subtler failure
//      where a returned view is in bounds of nothing in particular.
//
//   2. VALIDATION IS TRANSITIVE. If well_formed() accepts a document, then every
//      value reachable inside it must itself be a well-formed document. A
//      validator that accepts a whole but rejects one of its own parts is
//      wrong about one of them, and the guarantee callers rely on — "after
//      well_formed, an invalid find means the key is absent" — is only as good
//      as that transitivity.
//
//   3. classify() AGREES WITH THE TOKEN. The type reported must be the type the
//      first byte of the value implies, and for scalars the value must actually
//      spell that type. This is the invariant whose absence let
//      {"checksum":12ab} present as a number.
//
//   4. THE NUMBER GRAMMAR, DIFFERENTIALLY. is_json_number() is checked against
//      an independently written DFA for RFC 8259 §6. Two implementations of the
//      same grammar in different shapes disagree loudly when either drifts;
//      one implementation checked against itself never does.
//
//   5. STRING READING IS CONSISTENT. read_string, string_body and string_equals
//      must tell the same story about the same value, so a caller cannot pick
//      whichever accessor happens to give the answer it wants.
//
//   6. FIRST-WINS IS STABLE. Duplicate keys resolve to the first occurrence,
//      which neutralises a frame that names the same field twice with different
//      values. Fuzzing it means a rewrite cannot flip it under generated input
//      that the unit test did not think of.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "fuzz_check.hpp"

#include "crossbook/json.hpp"

using namespace crossbook;

namespace {

/// Depth cap for this file's own recursive walk. The scanner has its own limit
/// (json::kMaxDepth); this one only stops the oracle from out-recursing it.
constexpr int kWalkDepth = 40;

/// Is `span` entirely inside `input`? Written on pointers rather than offsets
/// because that is the property that actually matters: a view can have a
/// plausible size and still point at nothing the caller owns.
bool within(std::string_view span, std::string_view input) {
    if (span.empty()) {
        return true;  // An empty view carries no claim about its data pointer.
    }
    return span.data() >= input.data() &&
           span.data() + span.size() <= input.data() + input.size();
}

/// RFC 8259 §6, as an explicit DFA.
///
/// Deliberately a different shape from json::is_json_number's index walk. The
/// point of a differential oracle is that the two implementations do not share
/// a mistake; two copies of the same loop would agree on being wrong.
bool number_dfa(std::string_view t) {
    enum State { kStart, kSign, kZero, kInt, kDot, kFrac, kExp, kExpSign, kExpDigit, kDead };
    State st = kStart;
    for (const char c : t) {
        const bool digit = (c >= '0' && c <= '9');
        switch (st) {
            case kStart:
                st = (c == '-') ? kSign : (c == '0') ? kZero : (digit ? kInt : kDead);
                break;
            case kSign:
                st = (c == '0') ? kZero : (digit ? kInt : kDead);
                break;
            case kZero:
                // A leading zero may only be followed by a fraction or exponent;
                // "01" is not a JSON number and "0x10" is not anything.
                st = (c == '.') ? kDot : ((c == 'e' || c == 'E') ? kExp : kDead);
                break;
            case kInt:
                st = digit ? kInt : ((c == '.') ? kDot : ((c == 'e' || c == 'E') ? kExp : kDead));
                break;
            case kDot:
                st = digit ? kFrac : kDead;
                break;
            case kFrac:
                st = digit ? kFrac : ((c == 'e' || c == 'E') ? kExp : kDead);
                break;
            case kExp:
                st = (c == '+' || c == '-') ? kExpSign : (digit ? kExpDigit : kDead);
                break;
            case kExpSign:
                st = digit ? kExpDigit : kDead;
                break;
            case kExpDigit:
                st = digit ? kExpDigit : kDead;
                break;
            case kDead:
                return false;
        }
        if (st == kDead) {
            return false;
        }
    }
    return st == kZero || st == kInt || st == kFrac || st == kExpDigit;
}

/// Cross-check the scalar grammar against the independent DFA, on whatever
/// tokens the fuzzer produced plus the input itself.
void check_number_grammar(std::string_view token) {
    const bool mine = json::is_json_number(token);
    const bool theirs = number_dfa(token);
    CB_CHECK(mine == theirs);

    // Two spellings that are never JSON numbers however the grammar is written.
    if (mine) {
        CB_CHECK(!token.empty());
        CB_CHECK(token.front() != '+');
        CB_CHECK(token.front() == '-' || (token.front() >= '0' && token.front() <= '9'));
    }

    const bool scalar = json::is_json_scalar(token);
    CB_CHECK(scalar == (mine || token == "true" || token == "false" || token == "null"));
}

/// Every accessor must tell the same story about the same string value.
void check_string_accessors(const JsonValue& value, std::string_view input) {
    std::string_view body;
    const json::StringRead status = json::read_string(value, body);
    CB_CHECK(within(body, input));

    switch (status) {
        case json::StringRead::kNotAString:
            CB_CHECK(value.type != JsonType::kString || value.raw.size() < 2);
            CB_CHECK(body.empty());
            CB_CHECK(json::string_body(value).empty());
            break;
        case json::StringRead::kPlain:
            // The plain path is the only one where the raw span IS the value,
            // and string_body must agree with it exactly.
            CB_CHECK(json::string_body(value) == body);
            CB_CHECK(json::string_equals(value, body));
            CB_CHECK(body.size() + 2 == value.raw.size());
            break;
        case json::StringRead::kEscaped:
            // string_body's empty answer is only defensible because read_string
            // can tell the caller WHY it is empty. If those two ever disagree
            // the ambiguity is back.
            CB_CHECK(json::string_body(value).empty());
            CB_CHECK(!body.empty());
            break;
    }

    // Decoding only ever shrinks, so a value that compares equal to its own raw
    // body must have had no escapes in it. If an escaped value ever satisfied
    // this, string_equals would be handing back the undecoded bytes.
    if (!body.empty() && json::string_equals(value, body)) {
        CB_CHECK(status == json::StringRead::kPlain);
    }
}

/// Walk a validated value and assert the invariants that only hold once
/// well_formed() has said yes.
void check_validated(std::string_view value_raw, std::string_view input, int depth) {
    if (depth > kWalkDepth) {
        return;
    }
    CB_CHECK(within(value_raw, input));

    // Transitivity: a part of a valid document is itself a valid document.
    CB_CHECK(json::well_formed(value_raw));

    const JsonType type = json::classify(value_raw, 0);
    CB_CHECK(type != JsonType::kInvalid);

    switch (type) {
        case JsonType::kArray:
            CB_CHECK(value_raw.front() == '[');
            (void)json::for_each(value_raw, [&](const JsonValue& element) {
                CB_CHECK(element.type == json::classify(element.raw, 0));
                check_validated(element.raw, input, depth + 1);
                return true;
            });
            break;
        case JsonType::kObject:
            CB_CHECK(value_raw.front() == '{');
            break;
        case JsonType::kString:
            CB_CHECK(value_raw.front() == '"');
            CB_CHECK(value_raw.size() >= 2);
            CB_CHECK(value_raw.back() == '"');
            break;
        case JsonType::kNumber:
            // The scalar the classifier called a number must spell one. This is
            // the check whose absence turned {"checksum":12ab} into a kNumber
            // holding "12ab", which then failed to parse and switched checksum
            // verification off for the frame.
            CB_CHECK(json::is_json_number(value_raw));
            break;
        case JsonType::kBool:
            CB_CHECK(value_raw == "true" || value_raw == "false");
            break;
        case JsonType::kNull:
            CB_CHECK(value_raw == "null");
            break;
        case JsonType::kInvalid:
            break;
    }
}

/// Harvest key-shaped tokens out of the input so lookups are not confined to a
/// list of names written by hand. Whatever the fuzzer decided to put in the
/// document is what gets looked up in it.
std::vector<std::string_view> harvest_keys(std::string_view input) {
    std::vector<std::string_view> keys;
    keys.reserve(40);
    for (std::size_t i = 0; i < input.size() && keys.size() < 32; ++i) {
        if (input[i] != '"') {
            continue;
        }
        const std::size_t end = json::skip_string(input, i);
        if (end == std::string_view::npos) {
            break;  // Unterminated; nothing further can be a complete key.
        }
        CB_CHECK(end <= input.size());
        CB_CHECK(end >= i + 2);
        keys.push_back(input.substr(i + 1, end - i - 2));
        i = end - 1;
    }
    // Plus a handful the fuzzer is unlikely to synthesise, to keep the
    // venue-shaped paths exercised even on inputs that contain no strings.
    keys.push_back("channel");
    keys.push_back("checksum");
    keys.push_back("");
    return keys;
}

/// find() must return the FIRST match. Verified against an independent scan so
/// a rewrite of the loop cannot flip the tie-break silently.
void check_first_wins(std::string_view input, std::string_view key) {
    const JsonValue found = json::find(input, key);
    if (!found) {
        return;
    }
    // Re-scan from the front counting how many members carry this key; the span
    // find() returned must start at or before every other one of them.
    std::size_t pos = json::skip_space(input, 0);
    if (pos >= input.size() || input[pos] != '{') {
        return;
    }
    ++pos;
    while (pos < input.size()) {
        pos = json::skip_space(input, pos);
        if (pos >= input.size() || input[pos] == '}') {
            return;
        }
        const std::size_t key_start = pos;
        const std::size_t key_end = json::skip_string(input, pos);
        if (key_end == std::string_view::npos) {
            return;
        }
        pos = json::skip_space(input, key_end);
        if (pos >= input.size() || input[pos] != ':') {
            return;
        }
        ++pos;
        pos = json::skip_space(input, pos);
        const std::size_t value_start = pos;
        const std::size_t value_end = json::skip_value(input, pos);
        if (value_end == std::string_view::npos) {
            return;
        }
        if (input.substr(key_start + 1, key_end - key_start - 2) == key) {
            // The first occurrence is the only one that may be reported, and it
            // must be reported exactly — not merely "at or before".
            CB_CHECK(found.raw.data() == input.data() + value_start);
            return;
        }
        pos = json::skip_space(input, value_end);
        if (pos >= input.size() || input[pos] != ',') {
            return;
        }
        ++pos;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0 || size > 65536) {
        return 0;
    }
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    // The grammar oracle, on the input itself and on every span the scanner
    // will treat as a scalar run.
    check_number_grammar(input);

    // skip_value must never report a position past the end, whatever the bytes.
    const std::size_t end = json::skip_value(input, 0);
    CB_CHECK(end == std::string_view::npos || end <= input.size());

    const bool valid = json::well_formed(input);
    if (valid) {
        CB_CHECK(end != std::string_view::npos);
        CB_CHECK(json::skip_space(input, end) == input.size());
        check_validated(input, input, 0);
    }

    for (const std::string_view key : harvest_keys(input)) {
        const JsonValue value = json::find(input, key);
        if (!value) {
            continue;
        }
        CB_CHECK(within(value.raw, input));
        CB_CHECK(value.type == json::classify(value.raw, 0));

        // The guarantee well_formed exists to provide: once the document has
        // validated, anything find() reaches is itself valid.
        if (valid) {
            check_validated(value.raw, input, 1);
        }

        if (value.type == JsonType::kString) {
            check_string_accessors(value, input);
        }
        if (value.type == JsonType::kNumber) {
            check_number_grammar(value.raw);
            std::uint64_t parsed = 0;
            if (json::parse_u64(value.raw, parsed)) {
                // parse_u64 accepts digits only, so a token it accepted cannot
                // have carried a sign, a point, or an exponent.
                CB_CHECK(!value.raw.empty());
                for (const char c : value.raw) {
                    CB_CHECK(c >= '0' && c <= '9');
                }
            }
        }
        if (value.type == JsonType::kArray) {
            std::size_t elements = 0;
            (void)json::for_each(value.raw, [&](const JsonValue& element) {
                CB_CHECK(within(element.raw, input));
                CB_CHECK(element.type == json::classify(element.raw, 0));
                return ++elements < 1000;
            });
        }

        check_first_wins(input, key);
    }

    return 0;
}
