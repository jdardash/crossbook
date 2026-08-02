// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Differential fuzzing for the L3 book.
//
// This is the most bug-prone code in the library by some distance: intrusive
// doubly-linked lists, an index-based arena with a free list, and an
// open-addressed table with tombstones. Each of those has a failure mode that
// hand-written tests are poor at finding, because triggering it needs a
// specific interleaving of add, modify, cancel, and fill that a human does not
// think to write:
//
//   - a link/unlink that corrupts a neighbour's pointers
//   - a recycled arena slot still reachable from an old list
//   - a tombstone that severs a probe chain, making a live order unfindable
//
// The oracle is a deliberately naive model: a map of order id to order, and a
// map of price to total. Slow, obviously correct, and no shared code with the
// implementation. After every operation the real book's aggregate levels, order
// count, and per-order state must match it exactly.

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "fuzz_check.hpp"

#include "crossbook/l3.hpp"

using namespace crossbook;

namespace {

/// The naive model. No arena, no intrusive lists, no hashing.
struct Model {
    struct Entry {
        Side side;
        std::int64_t price;
        std::int64_t qty;
        std::uint64_t seq;  ///< Arrival order, standing in for queue position.
    };

    std::map<OrderId, Entry> orders;
    std::uint64_t next_seq{0};

    bool add(OrderId id, Side side, std::int64_t price, std::int64_t qty) {
        if (qty <= 0 || orders.count(id) != 0) {
            return false;
        }
        orders[id] = Entry{side, price, qty, next_seq++};
        return true;
    }

    bool modify(OrderId id, std::int64_t qty) {
        const auto it = orders.find(id);
        if (it == orders.end()) {
            return false;
        }
        if (qty <= 0) {
            orders.erase(it);
            return true;
        }
        if (qty > it->second.qty) {
            it->second.seq = next_seq++;  // An increase goes to the back.
        }
        it->second.qty = qty;
        return true;
    }

    bool remove(OrderId id) { return orders.erase(id) != 0; }

    bool execute(OrderId id, std::int64_t traded) {
        const auto it = orders.find(id);
        if (it == orders.end() || traded <= 0) {
            return false;
        }
        if (traded >= it->second.qty) {
            orders.erase(it);
        } else {
            it->second.qty -= traded;  // A fill never costs priority.
        }
        return true;
    }

    /// Aggregate levels for one side, in book order.
    [[nodiscard]] std::vector<Level> levels(Side side) const {
        std::map<std::int64_t, std::int64_t> totals;
        for (const auto& [id, entry] : orders) {
            if (entry.side == side) {
                totals[entry.price] += entry.qty;
            }
        }
        std::vector<Level> out;
        out.reserve(totals.size());
        if (side == Side::kAsk) {
            for (auto it = totals.begin(); it != totals.end(); ++it) {
                out.push_back(Level{Price{it->first}, Qty{it->second}});
            }
        } else {
            for (auto it = totals.rbegin(); it != totals.rend(); ++it) {
                out.push_back(Level{Price{it->first}, Qty{it->second}});
            }
        }
        return out;
    }

    /// Quantity ahead of `id` at its own price, by arrival order.
    [[nodiscard]] std::int64_t queue_ahead(OrderId id) const {
        const auto target = orders.find(id);
        if (target == orders.end()) {
            return -1;
        }
        std::int64_t ahead = 0;
        for (const auto& [other_id, entry] : orders) {
            if (entry.side == target->second.side && entry.price == target->second.price &&
                entry.seq < target->second.seq) {
                ahead += entry.qty;
            }
        }
        return ahead;
    }
};

void require_equivalent(const L3Book& book, const Model& model) {
    CB_CHECK(book.order_count() == model.orders.size());

    for (const Side side : {Side::kBid, Side::kAsk}) {
        std::vector<Level> actual;
        book.for_each_level(side, [&](const Level& level) {
            // A level that survives with zero quantity is a phantom price.
            CB_CHECK(level.qty.units > 0);
            actual.push_back(level);
            return true;
        });
        const std::vector<Level> expected = model.levels(side);
        CB_CHECK(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            CB_CHECK(actual[i].price == expected[i].price);
            CB_CHECK(actual[i].qty == expected[i].qty);
        }
    }

    // Every modelled order must be findable, with matching state and queue
    // position. This is what catches a severed probe chain or a stale link.
    for (const auto& [id, entry] : model.orders) {
        const Order* order = book.find(id);
        CB_CHECK(order != nullptr);
        CB_CHECK(order->qty.units == entry.qty);
        CB_CHECK(order->price.ticks == entry.price);
        CB_CHECK(order->side == entry.side);

        Qty ahead{};
        CB_CHECK(book.queue_ahead(id, ahead));
        CB_CHECK(ahead.units == model.queue_ahead(id));
    }
}

class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    [[nodiscard]] bool exhausted() const noexcept { return pos_ >= size_; }
    [[nodiscard]] std::uint8_t byte() noexcept { return pos_ < size_ ? data_[pos_++] : 0; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_{0};
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 4 || size > 65536) {
        return 0;
    }

    // A small arena and table force reallocation, rehashing, and slot reuse to
    // happen constantly rather than only in long runs.
    L3Book book(InstrumentSpec{"BTC/USD", 1, 8}, 16);
    Model model;
    Cursor cursor(data, size);

    while (!cursor.exhausted()) {
        const std::uint8_t op = cursor.byte();
        // A deliberately tiny id space so collisions, reuse, and duplicate-add
        // rejection are exercised rather than being astronomically unlikely.
        const auto id = static_cast<OrderId>(cursor.byte() % 64);
        const auto qty = static_cast<std::int64_t>(cursor.byte());

        switch (op % 5) {
            case 0: {
                const Side side = (op & 0x80) ? Side::kBid : Side::kAsk;
                const std::int64_t price = 452800 + (cursor.byte() % 32);
                CB_CHECK(book.add(id, side, Price{price}, Qty{qty}) ==
                         model.add(id, side, price, qty));
                break;
            }
            case 1:
                CB_CHECK(book.modify(id, Qty{qty}) == model.modify(id, qty));
                break;
            case 2:
                CB_CHECK(book.remove(id) == model.remove(id));
                break;
            case 3:
                CB_CHECK(book.execute(id, Qty{qty}) == model.execute(id, qty));
                break;
            case 4:
                // Clearing mid-stream exercises the resync path, where stale
                // arena slots or table entries would otherwise survive.
                book.clear();
                model.orders.clear();
                break;
            default:
                break;
        }

        require_equivalent(book, model);
    }

    return 0;
}
