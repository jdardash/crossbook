// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Differential fuzzing: ArrayBook against MapBook.
//
// The equivalence test in tests/ drives randomised streams shaped like a real
// book, which is the realistic case. This drives adversarial streams, which is
// the case that finds bugs: coverage-guided mutation will happily construct the
// exact sequence of window excursions, mass deletes, and re-anchors that a
// hand-written generator never would.
//
// The oracle is total. After every update, the full observable state of both
// books must be identical: hash, checksum, level counts, and the top 32 levels
// per side. Any divergence at all aborts.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fuzz_check.hpp"

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

namespace {

/// Pulls structured updates out of a flat byte string.
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] bool exhausted() const noexcept { return pos_ >= size_; }

    [[nodiscard]] std::uint8_t byte() noexcept { return pos_ < size_ ? data_[pos_++] : 0; }

    [[nodiscard]] std::int64_t price() noexcept {
        // Two bytes of offset around a fixed anchor keeps prices in a range
        // where the window is actually exercised, rather than scattering them
        // so widely that everything lands in the overflow map.
        const std::int64_t lo = byte();
        const std::int64_t hi = byte();
        return 452852 + ((hi << 8) | lo) - 32768;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_{0};
};

void require_equivalent(const MapBook& reference, const ArrayBook& subject) {
    CB_CHECK(reference.state_hash() == subject.state_hash());
    CB_CHECK(kraken_checksum(reference) == kraken_checksum(subject));
    CB_CHECK(reference.bids().size() == subject.bids().size());
    CB_CHECK(reference.asks().size() == subject.asks().size());

    std::vector<Level> a;
    std::vector<Level> b;
    for (const Side s : {Side::kBid, Side::kAsk}) {
        // Deeper than the checksum's 10 levels, so ordering errors below the
        // checksum horizon are still caught.
        const std::size_t na = reference.top(s, 32, a);
        const std::size_t nb = subject.top(s, 32, b);
        CB_CHECK(na == nb);
        CB_CHECK(a == b);

        Level la{};
        Level lb{};
        const bool ha = reference.best(s, la);
        const bool hb = subject.best(s, lb);
        CB_CHECK(ha == hb);
        CB_CHECK(!ha || la == lb);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 4 || size > 65536) {
        return 0;
    }

    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});
    Cursor cursor(data, size);

    while (!cursor.exhausted()) {
        const std::uint8_t op = cursor.byte();

        // A rare clear exercises the resync path, where stale residue would
        // otherwise hide until the next snapshot.
        if ((op & 0x3F) == 0x3F) {
            reference.clear();
            subject.clear();
            require_equivalent(reference, subject);
            continue;
        }

        const Side side = (op & 1) ? Side::kBid : Side::kAsk;
        const std::int64_t price = cursor.price();

        // Bias hard toward deletes. Removal is where the best-index hint has to
        // do real work, and it is the path most likely to be wrong.
        const std::uint8_t qty_byte = cursor.byte();
        const std::int64_t qty = (op & 2) ? 0 : static_cast<std::int64_t>(qty_byte) + 1;

        reference.apply(side, Price{price}, Qty{qty});
        subject.apply(side, Price{price}, Qty{qty});

        require_equivalent(reference, subject);
    }

    return 0;
}
