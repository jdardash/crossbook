// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Fuzz the JSON scanner and both venue decoders.
//
// This is the widest attack surface in the library: every byte here came off a
// socket, and the scanner is hand-written precisely so the checksum can see raw
// tokens. Hand-written parsing is where memory-safety bugs live, so it gets
// fuzzed with sanitizers on.
//
// Properties asserted beyond "does not crash":
//
//   - The scanner never reads outside its input, at any nesting depth.
//   - A decoder either produces a usable message or reports an error. It never
//     returns a message that claims to be ok while holding garbage.
//   - A message that decodes ok can be applied to a book without violating any
//     book invariant.
//   - Structural validation is consistent with navigation: if well_formed()
//     says no, the decoder must not report success.
//
// That last one is the invariant a real bug was found against. A truncated
// frame whose `"b":[[` array never closed used to decode as a perfectly valid
// update with no bids, and got applied.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "fuzz_check.hpp"

#include "crossbook/book.hpp"
#include "crossbook/json.hpp"
#include "crossbook/venues/binance.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;
using namespace crossbook::venues;

namespace {

/// Walk whatever structure the scanner can see, to exercise skipping, escape
/// handling, and the depth limit against arbitrary bytes.
void exercise_scanner(std::string_view input) {
    for (std::string_view key : {"channel", "type", "data", "e", "U", "u", "pu", "b", "a",
                                 "bids", "asks", "price", "qty", "checksum", "lastUpdateId"}) {
        const JsonValue value = json::find(input, key);
        if (!value) {
            continue;
        }
        // Every returned span must lie inside the input buffer.
        CB_CHECK(value.raw.data() >= input.data());
        CB_CHECK(value.raw.data() + value.raw.size() <= input.data() + input.size());

        if (value.type == JsonType::kArray) {
            std::size_t elements = 0;
            (void)json::for_each(value.raw, [&](const JsonValue& element) {
                CB_CHECK(element.raw.data() >= input.data());
                CB_CHECK(element.raw.data() + element.raw.size() <= input.data() + input.size());
                return ++elements < 1000;
            });
        }
        if (value.type == JsonType::kString) {
            const std::string_view body = json::string_body(value);
            CB_CHECK(body.size() <= value.raw.size());
        }
        if (value.type == JsonType::kNumber) {
            std::uint64_t parsed = 0;
            (void)json::parse_u64(value.raw, parsed);
        }
    }
}

/// A decoded message must be internally coherent and safe to apply.
void check_message(const DecodedMessage& msg, std::string_view input) {
    // Structural validation and navigation must agree, and this has to be
    // asserted BEFORE the !ok() early return.
    //
    // Below the return it was dead code, and dead in the way that is hardest to
    // notice: both decoders open with a well_formed() gate, so reaching the
    // check at all already implied well_formed(input) == true, and a guard
    // whose premise is unreachable checks nothing while looking like it checks
    // the most important thing in the file. This is the regression guard for
    // the truncated-frame bug — a `"b":[[` array that never closed decoding as
    // ok() with kind == kUpdate and no bids, then being applied as an empty
    // update — and it had been silently retired.
    //
    // Hoisted, it constrains the path that can actually go wrong: a frame that
    // is not well-formed must not produce a successful decode from ANY decoder,
    // and must not leave levels behind whatever status it reports.
    if (!json::well_formed(input)) {
        CB_CHECK(!msg.ok());
        CB_CHECK(msg.levels.empty());
    }

    if (!msg.ok()) {
        // A failed decode must not leave levels behind for a caller to apply.
        CB_CHECK(msg.levels.empty());
        return;
    }

    // A successful decode of a well-formed frame may be anything the venue
    // sends, but it may never claim more levels than the cap.
    CB_CHECK(msg.levels.size() <= kMaxLevelsPerMessage);
    if (!msg.symbol.empty()) {
        CB_CHECK(msg.symbol.data() >= input.data());
        CB_CHECK(msg.symbol.data() + msg.symbol.size() <= input.data() + input.size());
    }

    // Anything the decoder accepted must be applicable without breaking the
    // book, and both implementations must agree on the result.
    MapBook reference(InstrumentSpec{"X", 2, 8});
    ArrayBook subject(InstrumentSpec{"X", 2, 8});
    for (const LevelUpdate& level : msg.levels) {
        reference.apply(level.side, level.price, level.qty);
        subject.apply(level.side, level.price, level.qty);
    }
    CB_CHECK(reference.state_hash() == subject.state_hash());
    CB_CHECK(reference.bids().size() == subject.bids().size());
    CB_CHECK(reference.asks().size() == subject.asks().size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0 || size > 65536) {
        return 0;
    }
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    exercise_scanner(input);

    // Decoders are constructed once per input rather than once per process so a
    // crash reproduces from the input alone, with no carried-over state.
    KrakenBookDecoder kraken(InstrumentSpec{"BTC/USD", 1, 8});
    check_message(kraken.decode(input), input);

    BinanceDepthDecoder spot(InstrumentSpec{"BTCUSDT", 2, 8}, BinanceMarket::kSpot);
    check_message(spot.decode(input), input);

    BinanceDepthDecoder futures(InstrumentSpec{"BTCUSDT", 2, 8}, BinanceMarket::kFutures);
    check_message(futures.decode(input), input);

    BinanceDepthDecoder snapshot(InstrumentSpec{"BTCUSDT", 2, 8}, BinanceMarket::kSpot);
    check_message(snapshot.decode_snapshot(input), input);

    return 0;
}
