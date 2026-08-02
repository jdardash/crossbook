// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Binance spot SBE stream decoder — the binary wire, schema spot_stream 1:0.
//
// Binance has served its market data streams in Simple Binary Encoding since
// March 2025 (stream-sbe.binance.com, an Ed25519 API key required even for
// public data), and it is the only major spot venue with a binary L2 diff
// feed. The point of consuming it is blunt: the JSON decode that costs ~2 us
// and 75% of a frame becomes a handful of bounds-checked little-endian loads.
//
// This is a hand-rolled reader for one schema, not an SBE framework. The
// schema (sbe/schemas/stream_1_0.xml in binance-spot-api-docs) is four
// messages over fixed-width fields, two repeating groups, and one trailing
// varString8. What is honoured from the SBE spec: header blockLengths are
// trusted for forward compatibility, so a minor-version schema that appends
// root or entry fields skips cleanly rather than desyncing.
//
// Prices and quantities arrive as (int64 mantissa, int8 exponent) pairs. They
// are rescaled to the instrument's configured scales with the same refusal
// semantics as the text path: a value that does not land exactly on the
// instrument's grid is kPrecisionLoss, never rounded.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "crossbook/fixed.hpp"
#include "crossbook/sequence.hpp"
#include "crossbook/venue.hpp"

namespace crossbook::venues {

class BinanceSbeDecoder {
    // The wire is little-endian and the loads below are memcpy-then-use.
    // Every platform this project builds for is little-endian; if that ever
    // changes, this line is the compile error that says where to add swaps.
    static_assert(std::endian::native == std::endian::little,
                  "BinanceSbeDecoder assumes a little-endian host");

public:
    explicit BinanceSbeDecoder(InstrumentSpec spec) : spec_(std::move(spec)) {
        message_.levels.reserve(256);
    }

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }

    /// SBE streams exist for spot only; the spot sequencing rules apply.
    [[nodiscard]] SequencePolicy policy() const noexcept { return SequencePolicy::kBinanceSpot; }

    [[nodiscard]] const DecodedMessage& decode(std::string_view frame) {
        message_.reset();
        cursor_ = 0;
        frame_ = frame;

        std::uint16_t block_length = 0;
        std::uint16_t template_id = 0;
        std::uint16_t schema_id = 0;
        std::uint16_t version = 0;
        if (!read_u16(block_length) || !read_u16(template_id) || !read_u16(schema_id) ||
            !read_u16(version)) {
            return fail(DecodeError::kMalformed);
        }
        // A different schema id is a different wire format, and pretending to
        // read it would produce plausible garbage. Loudly malformed, so the
        // operator learns the venue rolled the schema.
        if (schema_id != kSchemaId) {
            return fail(DecodeError::kMalformed);
        }

        switch (template_id) {
            case kDepthDiffTemplate:
                return decode_depth(block_length, /*diff=*/true);
            case kDepthSnapshotTemplate:
                return decode_depth(block_length, /*diff=*/false);
            default:
                // Trades and bestBidAsk share the socket legitimately; a book
                // decoder ignoring them is routing, not failure.
                message_.kind = MessageKind::kIgnored;
                return message_;
        }
    }

private:
    static constexpr std::uint16_t kSchemaId = 1;
    static constexpr std::uint16_t kDepthSnapshotTemplate = 10002;
    static constexpr std::uint16_t kDepthDiffTemplate = 10003;
    /// int64 nanoseconds run out in April 2262; a microsecond event time past
    /// this cannot be scaled without overflow and is treated as absent, the
    /// same bound (in the venue's unit) the JSON decoder applies.
    static constexpr std::int64_t kMaxEventUs = 9'223'372'036'854'775LL;

    [[nodiscard]] const DecodedMessage& decode_depth(std::uint16_t block_length, bool diff) {
        const std::size_t root_start = cursor_;
        const std::size_t min_block = diff ? 26U : 18U;
        if (block_length < min_block || frame_.size() - cursor_ < block_length) {
            return fail(DecodeError::kMalformed);
        }

        std::int64_t event_us = 0;
        std::int64_t first_id = 0;
        std::int64_t last_id = 0;
        std::int8_t price_exp = 0;
        std::int8_t qty_exp = 0;
        (void)read_i64(event_us);
        if (diff) {
            (void)read_i64(first_id);
            (void)read_i64(last_id);
        } else {
            (void)read_i64(last_id);
            first_id = last_id;
        }
        (void)read_i8(price_exp);
        (void)read_i8(qty_exp);
        cursor_ = root_start + block_length;  // Skip fields a newer minor added.

        if (first_id < 0 || last_id < 0) {
            return fail(DecodeError::kBadSequence);
        }

        // The symbol trails the groups on the wire, but routing must happen
        // before any level is trusted. Group byte counts are arithmetic, so
        // peek ahead without decoding an entry.
        std::size_t peek = cursor_;
        std::string_view symbol;
        std::uint32_t bid_count = 0;
        std::uint32_t ask_count = 0;
        std::uint16_t bid_stride = 0;
        std::uint16_t ask_stride = 0;
        if (!peek_group(peek, bid_stride, bid_count) || !peek_group(peek, ask_stride, ask_count) ||
            !peek_symbol(peek, symbol)) {
            return fail(DecodeError::kMalformed);
        }
        message_.symbol = symbol;
        if (!symbol.empty() && !equals_ignore_case(symbol, spec_.symbol)) {
            message_.kind = MessageKind::kIgnored;
            message_.symbol = {};
            return message_;
        }

        const std::uint64_t total =
            static_cast<std::uint64_t>(bid_count) + static_cast<std::uint64_t>(ask_count);
        if (total > kMaxLevelsPerMessage) {
            return fail(DecodeError::kTooManyLevels);
        }

        message_.kind = diff ? MessageKind::kUpdate : MessageKind::kSnapshot;
        if (event_us > 0 && event_us <= kMaxEventUs) {
            message_.ts = static_cast<Timestamp>(event_us) * 1'000;
        }
        message_.ids.first_id = static_cast<SequenceId>(first_id);
        message_.ids.final_id = static_cast<SequenceId>(last_id);
        message_.has_ids = true;

        if (!decode_group(bid_stride, bid_count, price_exp, qty_exp, Side::kBid) ||
            !decode_group(ask_stride, ask_count, price_exp, qty_exp, Side::kAsk)) {
            return message_;  // decode_group set the error.
        }
        return message_;
    }

    /// Group dimensions from `at`, advancing it past the whole group.
    [[nodiscard]] bool peek_group(std::size_t& at, std::uint16_t& stride,
                                  std::uint32_t& count) const noexcept {
        std::uint16_t raw_count = 0;
        if (frame_.size() - at < 4) {
            return false;
        }
        std::memcpy(&stride, frame_.data() + at, 2);
        std::memcpy(&raw_count, frame_.data() + at + 2, 2);
        at += 4;
        count = raw_count;
        if (stride < 16) {
            return false;  // An entry is at least two int64s in every version.
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(stride) * count;
        if (frame_.size() - at < bytes) {
            return false;
        }
        at += static_cast<std::size_t>(bytes);
        return true;
    }

    [[nodiscard]] bool peek_symbol(std::size_t at, std::string_view& out) const noexcept {
        if (at >= frame_.size()) {
            return false;
        }
        const auto len = static_cast<std::uint8_t>(frame_[at]);
        if (frame_.size() - at - 1 < len) {
            return false;
        }
        out = frame_.substr(at + 1, len);
        return true;
    }

    [[nodiscard]] bool decode_group(std::uint16_t stride, std::uint32_t count,
                                    std::int8_t price_exp, std::int8_t qty_exp, Side side) {
        cursor_ += 4;  // The dimensions peek_group already validated.
        for (std::uint32_t i = 0; i < count; ++i) {
            std::int64_t price_mantissa = 0;
            std::int64_t qty_mantissa = 0;
            std::memcpy(&price_mantissa, frame_.data() + cursor_, 8);
            std::memcpy(&qty_mantissa, frame_.data() + cursor_ + 8, 8);
            cursor_ += stride;

            LevelUpdate level{};
            level.side = side;
            if (price_mantissa < 0 || qty_mantissa < 0) {
                (void)fail(DecodeError::kMalformed);
                return false;
            }
            if (!rescale(price_mantissa, price_exp, spec_.price_scale, level.price.ticks) ||
                !rescale(qty_mantissa, qty_exp, spec_.qty_scale, level.qty.units)) {
                (void)fail(DecodeError::kPrecisionLoss);
                return false;
            }
            message_.levels.push_back(level);
        }
        return true;
    }

    /// mantissa x 10^exponent, re-expressed as an integer at `scale`.
    /// Exact or refused: the same contract parse_fixed enforces for text.
    [[nodiscard]] static bool rescale(std::int64_t mantissa, std::int8_t exponent, Scale scale,
                                      std::int64_t& out) noexcept {
        const int shift = static_cast<int>(scale) + static_cast<int>(exponent);
        if (shift == 0) {
            out = mantissa;
            return true;
        }
        if (shift > 0) {
            if (shift > static_cast<int>(kMaxScale)) {
                return false;
            }
            return detail::checked_mul(mantissa, kPow10[static_cast<std::size_t>(shift)], out);
        }
        if (-shift > static_cast<int>(kMaxScale)) {
            if (mantissa == 0) {
                out = 0;
                return true;
            }
            return false;
        }
        const std::int64_t divisor = kPow10[static_cast<std::size_t>(-shift)];
        if (mantissa % divisor != 0) {
            return false;
        }
        out = mantissa / divisor;
        return true;
    }

    [[nodiscard]] const DecodedMessage& fail(DecodeError error) {
        message_.kind = MessageKind::kIgnored;
        message_.error = error;
        message_.levels.clear();
        return message_;
    }

    [[nodiscard]] bool read_u16(std::uint16_t& out) noexcept {
        if (frame_.size() - cursor_ < 2) {
            return false;
        }
        std::memcpy(&out, frame_.data() + cursor_, 2);
        cursor_ += 2;
        return true;
    }

    [[nodiscard]] bool read_i64(std::int64_t& out) noexcept {
        if (frame_.size() - cursor_ < 8) {
            return false;
        }
        std::memcpy(&out, frame_.data() + cursor_, 8);
        cursor_ += 8;
        return true;
    }

    [[nodiscard]] bool read_i8(std::int8_t& out) noexcept {
        if (frame_.size() - cursor_ < 1) {
            return false;
        }
        std::memcpy(&out, frame_.data() + cursor_, 1);
        cursor_ += 1;
        return true;
    }

    [[nodiscard]] static bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto lower = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            };
            if (lower(a[i]) != lower(b[i])) {
                return false;
            }
        }
        return true;
    }

    DecodedMessage message_;
    InstrumentSpec spec_;
    std::string_view frame_;
    std::size_t cursor_{0};
};

}  // namespace crossbook::venues
