// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Fuzz the decimal parser.
//
// Parsers are where memory-safety bugs live, and this one runs on every price
// and quantity on every message from every venue. It is the widest attack
// surface in the library and the only place that touches untrusted bytes
// directly, so it gets fuzzed first.
//
// Properties asserted, beyond "does not crash":
//   - a successful parse always round-trips through format_fixed
//   - a rejected parse never leaves a non-zero mantissa behind
//   - parsing is total: no input, however malformed, may abort or hang

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fuzz_check.hpp"

#include "crossbook/fixed.hpp"

using namespace crossbook;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }

    // First byte picks the scale; the rest is the candidate token. Deriving the
    // scale from the input lets the fuzzer explore the scale/precision
    // interaction, which is where the interesting failures are.
    const Scale scale = static_cast<Scale>(data[0] % (kMaxScale + 2));  // +2 to reach invalid.
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);

    const ParseResult result = parse_fixed(text, scale);

    if (!result.ok()) {
        // A rejected parse must not smuggle out a value.
        CB_CHECK(result.mantissa == 0);
        return 0;
    }

    CB_CHECK(scale <= kMaxScale);

    // Round-trip: format the mantissa and re-parse it. This must be exact.
    // If it is not, the checksum path is unsound, because it assumes the
    // mantissa is a faithful representation of what arrived on the wire.
    const std::string canonical = format_fixed(result.mantissa, scale);
    const ParseResult reparsed = parse_fixed(canonical, scale);
    CB_CHECK(reparsed.ok());
    CB_CHECK(reparsed.mantissa == result.mantissa);

    // The checksum token must be pure digits: anything else corrupts the CRC
    // input and would produce mismatches that look like book errors.
    const std::string token = checksum_token(result.mantissa);
    CB_CHECK(!token.empty());
    for (char c : token) {
        CB_CHECK(c >= '0' && c <= '9');
    }

    return 0;
}
