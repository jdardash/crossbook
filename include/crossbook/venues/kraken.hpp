// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Kraken WebSocket v2 `book` channel decoder.
//
// Message shape (docs.kraken.com/api/docs/websocket-v2/book):
//
//   {"channel":"book","type":"snapshot"|"update",
//    "data":[{"symbol":"BTC/USD",
//             "bids":[{"price":45283.5,"qty":0.5}, ...],
//             "asks":[{"price":45284.1,"qty":1.2}, ...],
//             "checksum":2418130093,
//             "timestamp":"2026-07-31T12:00:00.000000Z"}]}
//
// Two properties make this the anchor venue:
//
//   - It is public. No API key, so the correctness claim is reproducible by
//     anyone who clones the repository.
//   - Every message carries a CRC32 of the top 10 levels, so the book is
//     verified against the exchange's own arithmetic rather than against
//     itself.
//
// Prices and quantities arrive as JSON *numbers*. They are read as raw tokens
// and converted to exact fixed-point mantissas, never through a double — see
// fixed.hpp for why that is load-bearing rather than fastidious.

#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "crossbook/fixed.hpp"
#include "crossbook/json.hpp"
#include "crossbook/venue.hpp"

namespace crossbook::venues {

/// Decoder for Kraken's v2 book channel.
///
/// Holds the output message so its level buffer is reused across calls; steady
/// state performs no allocation.
class KrakenBookDecoder {
public:
    explicit KrakenBookDecoder(InstrumentSpec spec) : spec_(std::move(spec)) {
        message_.levels.reserve(256);
    }

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }

    /// Decode one frame. The returned reference is valid until the next call,
    /// and its string views alias `frame`.
    [[nodiscard]] const DecodedMessage& decode(std::string_view frame) {
        message_.reset();

        // Structural validation first: see json::well_formed. Without it a
        // truncated frame decodes as a valid update with the missing side
        // simply absent, and gets applied.
        if (!json::well_formed(frame)) {
            return fail(DecodeError::kMalformed);
        }

        const JsonValue channel = json::find(frame, "channel");
        if (!channel) {
            // Not a channel message at all — an ack, a pong, or a status
            // frame. Not an error; simply not ours.
            message_.kind = MessageKind::kIgnored;
            return message_;
        }
        const std::string_view channel_name = json::string_body(channel);
        if (channel_name == "status" || channel_name == "heartbeat") {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }
        if (channel_name != "book") {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }

        const JsonValue type = json::find(frame, "type");
        const std::string_view type_name = json::string_body(type);
        if (type_name == "snapshot") {
            message_.kind = MessageKind::kSnapshot;
        } else if (type_name == "update") {
            message_.kind = MessageKind::kUpdate;
        } else {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }

        const JsonValue data = json::find(frame, "data");
        if (!data || data.type != JsonType::kArray) {
            return fail(DecodeError::kMalformed);
        }

        bool saw_entry = false;
        bool failed = false;
        const bool walked = json::for_each(data.raw, [&](const JsonValue& entry) {
            if (entry.type != JsonType::kObject) {
                failed = true;
                return false;
            }
            // One instrument per decoder. A multiplexed frame is possible in
            // principle; routing belongs above this layer.
            if (saw_entry) {
                return false;
            }
            saw_entry = true;
            failed = !decode_entry(entry.raw);
            return !failed;
        });

        if (!walked || failed) {
            return message_.ok() ? fail(DecodeError::kMalformed) : message_;
        }
        if (!saw_entry) {
            return fail(DecodeError::kMalformed);
        }
        return message_;
    }

private:
    [[nodiscard]] const DecodedMessage& fail(DecodeError e) noexcept {
        message_.error = e;
        message_.levels.clear();
        return message_;
    }

    [[nodiscard]] bool decode_entry(std::string_view entry) {
        message_.symbol = json::string_body(json::find(entry, "symbol"));

        // Kraken sends the checksum as an unsigned 32-bit value. Its absence on
        // an update is not fatal — verification simply cannot run for it.
        const JsonValue checksum = json::find(entry, "checksum");
        if (checksum && checksum.type == JsonType::kNumber) {
            std::uint64_t raw = 0;
            if (json::parse_u64(checksum.raw, raw) && raw <= 0xFFFFFFFFULL) {
                message_.checksum = static_cast<std::uint32_t>(raw);
                message_.has_checksum = true;
            }
        }

        if (!decode_side(entry, "bids", Side::kBid)) {
            return false;
        }
        if (!decode_side(entry, "asks", Side::kAsk)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool decode_side(std::string_view entry, std::string_view key, Side side) {
        const JsonValue array = json::find(entry, key);
        if (!array) {
            return true;  // A one-sided update is normal.
        }
        if (array.type != JsonType::kArray) {
            return false;
        }

        bool failed = false;
        const bool walked = json::for_each(array.raw, [&](const JsonValue& level) {
            if (level.type != JsonType::kObject) {
                failed = true;
                return false;
            }
            if (message_.levels.size() >= kMaxLevelsPerMessage) {
                message_.error = DecodeError::kTooManyLevels;
                failed = true;
                return false;
            }

            const std::string_view price_token = json::number_token(json::find(level.raw, "price"));
            const std::string_view qty_token = json::number_token(json::find(level.raw, "qty"));
            if (price_token.empty() || qty_token.empty()) {
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
    DecodedMessage message_;
};

}  // namespace crossbook::venues
