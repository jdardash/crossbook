// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Binance diff-depth decoder, spot and futures.
//
// Spot message shape:
//
//   {"e":"depthUpdate","E":1571889248277,"s":"BTCUSDT",
//    "U":157,"u":160,
//    "b":[["0.0024","10"]],"a":[["0.0026","100"]]}
//
// Futures (USDS-M and coin-margined) adds a transaction time and, critically,
// `pu`:
//
//   {"e":"depthUpdate","E":...,"T":...,"s":"BTCUSDT",
//    "U":157,"u":160,"pu":149, "b":[...],"a":[...]}
//
// THE DIFFERENCE THAT MATTERS:
//
//   spot     continuity is  U == previous u + 1
//   futures  continuity is  pu == previous u
//
// These are not interchangeable. Applying the spot rule to a futures stream
// reports constant phantom gaps, and applying the futures rule to spot silently
// accepts real ones. The policy is therefore explicit at construction and
// asserted by a test that drives the same event through both and requires
// opposite verdicts.
//
// Binance publishes NO checksum. Correctness here rests entirely on sequence
// continuity plus REST snapshot reconciliation — which is why sequence.hpp is
// not an optional extra.
//
// Prices and quantities arrive as JSON *strings* rather than numbers, unlike
// Kraken. Both are handled by reading the raw token.

#pragma once

#include <string_view>

#include "crossbook/fixed.hpp"
#include "crossbook/json.hpp"
#include "crossbook/venue.hpp"

namespace crossbook::venues {

/// Which Binance market a stream belongs to. Selects the continuity contract.
enum class BinanceMarket : std::uint8_t {
    kSpot,
    kFutures,
};

/// Decoder for Binance diff-depth streams.
class BinanceDepthDecoder {
public:
    BinanceDepthDecoder(InstrumentSpec spec, BinanceMarket market)
        : spec_(std::move(spec)), market_(market) {
        message_.levels.reserve(256);
    }

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] BinanceMarket market() const noexcept { return market_; }

    /// The continuity policy this stream requires. Handed to SequenceTracker so
    /// the two cannot drift apart.
    [[nodiscard]] SequencePolicy policy() const noexcept {
        return market_ == BinanceMarket::kSpot ? SequencePolicy::kBinanceSpot
                                               : SequencePolicy::kBinanceFutures;
    }

    /// Decode one frame. The returned reference is valid until the next call.
    [[nodiscard]] const DecodedMessage& decode(std::string_view frame) {
        message_.reset();

        // Structural validation first: see json::well_formed. A truncated
        // frame must not decode as an update whose missing side reads as an
        // empty one.
        if (!json::well_formed(frame)) {
            return fail(DecodeError::kMalformed);
        }

        // Combined streams wrap the payload: {"stream":"...","data":{...}}.
        std::string_view payload = frame;
        const JsonValue wrapped = json::find(frame, "data");
        if (wrapped && wrapped.type == JsonType::kObject) {
            payload = wrapped.raw;
        }

        const JsonValue event = json::find(payload, "e");
        if (!event) {
            message_.kind = MessageKind::kIgnored;  // Ack or pong.
            return message_;
        }
        if (json::string_body(event) != "depthUpdate") {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }
        message_.kind = MessageKind::kUpdate;
        message_.symbol = json::string_body(json::find(payload, "s"));

        // Event time, milliseconds since epoch. Normalised to nanoseconds so
        // no consumer has to remember which venue used which unit.
        std::uint64_t event_ms = 0;
        if (json::parse_u64(json::number_token(json::find(payload, "E")), event_ms)) {
            message_.ts = static_cast<Timestamp>(event_ms) * 1'000'000;
        }

        std::uint64_t first_id = 0;
        std::uint64_t final_id = 0;
        if (!json::parse_u64(json::number_token(json::find(payload, "U")), first_id) ||
            !json::parse_u64(json::number_token(json::find(payload, "u")), final_id)) {
            return fail(DecodeError::kBadSequence);
        }
        message_.ids.first_id = first_id;
        message_.ids.final_id = final_id;

        if (market_ == BinanceMarket::kFutures) {
            std::uint64_t prev_id = 0;
            if (!json::parse_u64(json::number_token(json::find(payload, "pu")), prev_id)) {
                // A futures stream without `pu` cannot have its continuity
                // checked at all. Failing here is the whole point: silently
                // degrading to "no gap detection" is how a book drifts.
                return fail(DecodeError::kBadSequence);
            }
            message_.ids.prev_id = prev_id;
        }
        message_.has_ids = true;

        if (!decode_side(payload, "b", Side::kBid)) {
            return message_.ok() ? fail(DecodeError::kMalformed) : message_;
        }
        if (!decode_side(payload, "a", Side::kAsk)) {
            return message_.ok() ? fail(DecodeError::kMalformed) : message_;
        }
        return message_;
    }

    /// Decode a REST depth snapshot: {"lastUpdateId":160,"bids":[...],"asks":[...]}
    ///
    /// Reconciling this against the buffered diff stream is the only way to
    /// start a Binance book correctly, since the stream carries no images.
    [[nodiscard]] const DecodedMessage& decode_snapshot(std::string_view body) {
        message_.reset();
        message_.kind = MessageKind::kSnapshot;
        if (!json::well_formed(body)) {
            return fail(DecodeError::kMalformed);
        }

        std::uint64_t last_update_id = 0;
        if (!json::parse_u64(json::number_token(json::find(body, "lastUpdateId")),
                             last_update_id)) {
            return fail(DecodeError::kBadSequence);
        }
        message_.ids.first_id = last_update_id;
        message_.ids.final_id = last_update_id;
        message_.ids.prev_id = last_update_id;
        message_.has_ids = true;

        if (!decode_side(body, "bids", Side::kBid)) {
            return message_.ok() ? fail(DecodeError::kMalformed) : message_;
        }
        if (!decode_side(body, "asks", Side::kAsk)) {
            return message_.ok() ? fail(DecodeError::kMalformed) : message_;
        }
        return message_;
    }

private:
    [[nodiscard]] const DecodedMessage& fail(DecodeError e) noexcept {
        message_.error = e;
        message_.levels.clear();
        return message_;
    }

    /// Levels are two-element arrays of numeric strings: ["price","qty"].
    [[nodiscard]] bool decode_side(std::string_view payload, std::string_view key, Side side) {
        const JsonValue array = json::find(payload, key);
        if (!array) {
            return true;  // One-sided updates are normal.
        }
        if (array.type != JsonType::kArray) {
            return false;
        }

        bool failed = false;
        const bool walked = json::for_each(array.raw, [&](const JsonValue& level) {
            if (level.type != JsonType::kArray) {
                failed = true;
                return false;
            }
            if (message_.levels.size() >= kMaxLevelsPerMessage) {
                message_.error = DecodeError::kTooManyLevels;
                failed = true;
                return false;
            }

            std::string_view price_token;
            std::string_view qty_token;
            int seen = 0;
            const bool pair_ok = json::for_each(level.raw, [&](const JsonValue& field) {
                if (seen == 0) {
                    price_token = json::number_token(field);
                } else if (seen == 1) {
                    qty_token = json::number_token(field);
                }
                ++seen;
                return seen < 2;  // Ignore any trailing elements.
            });
            if (!pair_ok || price_token.empty() || qty_token.empty()) {
                failed = true;
                return false;
            }

            const ParseResult price = parse_fixed(price_token, spec_.price_scale);
            if (!price.ok()) {
                message_.error = (price.error == ParseError::kPrecisionLoss)
                                     ? DecodeError::kPrecisionLoss
                                     : DecodeError::kMalformed;
                message_.bad_token = price_token;
                failed = true;
                return false;
            }
            const ParseResult qty = parse_fixed(qty_token, spec_.qty_scale);
            if (!qty.ok()) {
                message_.error = (qty.error == ParseError::kPrecisionLoss)
                                     ? DecodeError::kPrecisionLoss
                                     : DecodeError::kMalformed;
                message_.bad_token = qty_token;
                failed = true;
                return false;
            }

            message_.levels.push_back(LevelUpdate{side, Price{price.mantissa}, Qty{qty.mantissa}});
            return true;
        });

        return walked && !failed;
    }

    InstrumentSpec spec_;
    BinanceMarket market_;
    DecodedMessage message_;
};

}  // namespace crossbook::venues
