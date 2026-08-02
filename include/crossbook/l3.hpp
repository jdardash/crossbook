// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// L3 (order-by-order) book.
//
// WHAT L3 BUYS YOU THAT L2 CANNOT
//
// An L2 book knows there are 5 coins resting at 45283.6. An L3 book knows they
// are seven separate orders in a specific queue, which is the difference
// between "there is size there" and "if I join now, 4.2 coins trade ahead of
// me". Queue position is not derivable from aggregated depth at any resolution,
// and for anything resting on the book it is the number that decides whether a
// strategy works.
//
// STRUCTURE
//
//   - Orders live in an arena, indexed rather than pointed to, so the pool can
//     grow without invalidating anything and a node reference survives a
//     reallocation.
//   - Each price level owns an intrusive doubly-linked list of its orders in
//     queue order. Intrusive because cancelling is the most common operation on
//     any real book, and unlinking a known node has to be O(1) with no search.
//   - order_id lookup is an open-addressed table rather than std::unordered_map:
//     one cache line per probe instead of a pointer chase per bucket, and no
//     per-node allocation.
//
// QUEUE POSITION SEMANTICS
//
// A size *reduction* keeps queue priority on every venue that matters; a size
// *increase* loses it and goes to the back. Getting that backwards silently
// inflates every fill estimate a strategy makes, so `modify` implements both
// and the distinction is tested rather than assumed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "crossbook/types.hpp"

namespace crossbook {

/// Venue-assigned order identifier.
using OrderId = std::uint64_t;

/// Index into the order arena. A sentinel stands in for "none" so the hot path
/// never dereferences a null pointer.
using OrderRef = std::uint32_t;
inline constexpr OrderRef kNoOrder = (std::numeric_limits<OrderRef>::max)();

/// One resting order.
struct Order {
    OrderId id{0};
    Price price{};
    Qty qty{};
    Side side{};
    /// Intrusive list links within this order's price level, in queue order.
    OrderRef prev{kNoOrder};
    OrderRef next{kNoOrder};
    bool live{false};
};

/// Aggregate state of one price level, maintained incrementally.
struct L3Level {
    Qty qty{};              ///< Sum of resting quantity.
    std::uint32_t orders{0};///< Number of resting orders.
    OrderRef head{kNoOrder};///< Front of the queue: first to trade.
    OrderRef tail{kNoOrder};///< Back of the queue.
};

/// Order-by-order book for one instrument.
///
/// Not thread-safe: one feed thread owns it. Making it safe would mean locking
/// the hot path, which defeats the point.
class L3Book {
public:
    explicit L3Book(InstrumentSpec spec, std::size_t expected_orders = 4096)
        : spec_(std::move(spec)) {
        orders_.reserve(expected_orders);
        free_list_.reserve(expected_orders / 4);
        rehash(next_pow2(expected_orders * 2));
    }

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return live_orders_; }

    /// Add a new resting order to the back of its price level's queue.
    ///
    /// Returns false if the id is already live. A venue re-adding a live id is
    /// a protocol violation, and treating it as an update would corrupt the
    /// aggregate quantity in a way nothing downstream could detect.
    bool add(OrderId id, Side side, Price price, Qty qty) {
        if (qty.units <= 0 || find_ref(id) != kNoOrder) {
            return false;
        }

        const OrderRef ref = allocate();
        Order& order = orders_[ref];
        order.id = id;
        order.side = side;
        order.price = price;
        order.qty = qty;
        order.prev = kNoOrder;
        order.next = kNoOrder;
        order.live = true;

        link_back(ref);
        insert_ref(id, ref);
        ++live_orders_;
        return true;
    }

    /// Change an order's resting quantity.
    ///
    /// A reduction keeps queue position; an increase forfeits it and moves the
    /// order to the back. `new_qty` of zero removes the order outright.
    bool modify(OrderId id, Qty new_qty) {
        const OrderRef ref = find_ref(id);
        if (ref == kNoOrder) {
            return false;
        }
        if (new_qty.units <= 0) {
            return remove(id);
        }

        Order& order = orders_[ref];
        const Qty old_qty = order.qty;
        L3Level& level = level_for(order.side, order.price);

        level.qty.units += (new_qty.units - old_qty.units);
        order.qty = new_qty;

        if (new_qty.units > old_qty.units) {
            // Priority is lost on an increase. Re-queue at the back.
            unlink(ref);
            link_back(ref);
        }
        return true;
    }

    /// Remove an order: a cancel, or the tail of a full fill.
    bool remove(OrderId id) {
        const OrderRef ref = find_ref(id);
        if (ref == kNoOrder) {
            return false;
        }
        unlink(ref);
        erase_ref(id);
        orders_[ref].live = false;
        free_list_.push_back(ref);
        --live_orders_;
        return true;
    }

    /// Reduce an order by a traded quantity, removing it if fully filled.
    ///
    /// Distinct from `modify` because a partial fill always retains priority,
    /// whereas a modify to the same size might not have.
    bool execute(OrderId id, Qty traded) {
        const OrderRef ref = find_ref(id);
        if (ref == kNoOrder || traded.units <= 0) {
            return false;
        }
        Order& order = orders_[ref];
        if (traded.units >= order.qty.units) {
            return remove(id);
        }
        level_for(order.side, order.price).qty.units -= traded.units;
        order.qty.units -= traded.units;
        return true;
    }

    /// Drop everything. Used on resync, for the same reason as the L2 book.
    void clear() noexcept {
        orders_.clear();
        free_list_.clear();
        bids_.clear();
        asks_.clear();
        for (auto& slot : table_) {
            slot = Slot{};
        }
        live_orders_ = 0;
        table_used_ = 0;
    }

    /// Look up a live order.
    [[nodiscard]] const Order* find(OrderId id) const noexcept {
        const OrderRef ref = find_ref(id);
        return ref == kNoOrder ? nullptr : &orders_[ref];
    }

    /// THE QUESTION ONLY L3 CAN ANSWER.
    ///
    /// Total quantity resting ahead of `id` at its own price level — the volume
    /// that must trade before this order does. Returns false if the id is not
    /// live.
    [[nodiscard]] bool queue_ahead(OrderId id, Qty& out) const noexcept {
        const OrderRef ref = find_ref(id);
        if (ref == kNoOrder) {
            return false;
        }
        const Order& target = orders_[ref];
        const L3Level* level = find_level(target.side, target.price);
        if (level == nullptr) {
            return false;
        }

        std::int64_t ahead = 0;
        for (OrderRef cursor = level->head; cursor != kNoOrder && cursor != ref;
             cursor = orders_[cursor].next) {
            ahead += orders_[cursor].qty.units;
        }
        out = Qty{ahead};
        return true;
    }

    /// Aggregate state at a price, or nullptr if the level is empty.
    [[nodiscard]] const L3Level* find_level(Side side, Price price) const noexcept {
        const auto& levels = (side == Side::kBid) ? bids_ : asks_;
        const auto it = levels.find(price.ticks);
        return it == levels.end() ? nullptr : &it->second;
    }

    /// Best price and aggregate size on a side.
    [[nodiscard]] bool best(Side side, Level& out) const noexcept {
        const auto& levels = (side == Side::kBid) ? bids_ : asks_;
        if (levels.empty()) {
            return false;
        }
        const auto it = (side == Side::kBid) ? std::prev(levels.end()) : levels.begin();
        out = Level{Price{it->first}, it->second.qty};
        return true;
    }

    /// Visit aggregated levels in book order until `fn` returns false.
    ///
    /// This is the L2 projection of the L3 book. Keeping it derived rather than
    /// maintained separately means the two can never disagree — a second
    /// aggregate updated in parallel is a second thing to get wrong.
    template <typename Fn>
    void for_each_level(Side side, Fn&& fn) const {
        const auto& levels = (side == Side::kBid) ? bids_ : asks_;
        if (side == Side::kBid) {
            for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
                if (!fn(Level{Price{it->first}, it->second.qty})) {
                    return;
                }
            }
        } else {
            for (auto it = levels.begin(); it != levels.end(); ++it) {
                if (!fn(Level{Price{it->first}, it->second.qty})) {
                    return;
                }
            }
        }
    }

    /// Visit the orders at a price level in queue order.
    template <typename Fn>
    void for_each_order(Side side, Price price, Fn&& fn) const {
        const L3Level* level = find_level(side, price);
        if (level == nullptr) {
            return;
        }
        for (OrderRef cursor = level->head; cursor != kNoOrder; cursor = orders_[cursor].next) {
            if (!fn(orders_[cursor])) {
                return;
            }
        }
    }

    /// Arena slots currently allocated, live or recycled. Diagnostic: a pool
    /// that only ever grows is a leak wearing a hat.
    [[nodiscard]] std::size_t arena_size() const noexcept { return orders_.size(); }
    [[nodiscard]] std::size_t free_slots() const noexcept { return free_list_.size(); }

private:
    struct Slot {
        OrderId id{0};
        OrderRef ref{kNoOrder};
        bool occupied{false};
        bool tombstone{false};
    };

    [[nodiscard]] static std::size_t next_pow2(std::size_t n) noexcept {
        std::size_t p = 16;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    /// Mixing constant from splitmix64. Venue order ids are frequently
    /// sequential, and a raw modulo on sequential keys clusters every probe
    /// into adjacent slots.
    [[nodiscard]] static std::uint64_t mix(OrderId id) noexcept {
        std::uint64_t z = id + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    [[nodiscard]] OrderRef find_ref(OrderId id) const noexcept {
        if (table_.empty()) {
            return kNoOrder;
        }
        const std::size_t mask = table_.size() - 1;
        std::size_t i = static_cast<std::size_t>(mix(id)) & mask;
        for (std::size_t probes = 0; probes <= mask; ++probes) {
            const Slot& slot = table_[i];
            if (!slot.occupied && !slot.tombstone) {
                return kNoOrder;  // A never-used slot terminates the probe.
            }
            if (slot.occupied && slot.id == id) {
                return slot.ref;
            }
            i = (i + 1) & mask;
        }
        return kNoOrder;
    }

    void insert_ref(OrderId id, OrderRef ref) {
        // Grow at 70% load. Linear probing degrades sharply past that, and a
        // feed handler cannot afford a probe storm during a burst.
        if ((table_used_ + 1) * 10 >= table_.size() * 7) {
            rehash(table_.size() * 2);
        }
        const std::size_t mask = table_.size() - 1;
        std::size_t i = static_cast<std::size_t>(mix(id)) & mask;
        while (table_[i].occupied) {
            i = (i + 1) & mask;
        }
        if (!table_[i].tombstone) {
            ++table_used_;
        }
        table_[i] = Slot{id, ref, true, false};
    }

    void erase_ref(OrderId id) noexcept {
        const std::size_t mask = table_.size() - 1;
        std::size_t i = static_cast<std::size_t>(mix(id)) & mask;
        for (std::size_t probes = 0; probes <= mask; ++probes) {
            Slot& slot = table_[i];
            if (!slot.occupied && !slot.tombstone) {
                return;
            }
            if (slot.occupied && slot.id == id) {
                slot.occupied = false;
                slot.tombstone = true;  // Keeps later probe chains intact.
                return;
            }
            i = (i + 1) & mask;
        }
    }

    void rehash(std::size_t capacity) {
        std::vector<Slot> old;
        old.swap(table_);
        table_.assign(capacity, Slot{});
        table_used_ = 0;
        for (const Slot& slot : old) {
            if (slot.occupied) {
                const std::size_t mask = table_.size() - 1;
                std::size_t i = static_cast<std::size_t>(mix(slot.id)) & mask;
                while (table_[i].occupied) {
                    i = (i + 1) & mask;
                }
                table_[i] = Slot{slot.id, slot.ref, true, false};
                ++table_used_;
            }
        }
    }

    [[nodiscard]] OrderRef allocate() {
        if (!free_list_.empty()) {
            const OrderRef ref = free_list_.back();
            free_list_.pop_back();
            return ref;
        }
        orders_.emplace_back();
        return static_cast<OrderRef>(orders_.size() - 1);
    }

    [[nodiscard]] L3Level& level_for(Side side, Price price) {
        auto& levels = (side == Side::kBid) ? bids_ : asks_;
        return levels[price.ticks];
    }

    /// Append to the back of the queue at this order's price.
    void link_back(OrderRef ref) {
        Order& order = orders_[ref];
        L3Level& level = level_for(order.side, order.price);

        order.prev = level.tail;
        order.next = kNoOrder;
        if (level.tail != kNoOrder) {
            orders_[level.tail].next = ref;
        } else {
            level.head = ref;
        }
        level.tail = ref;
        level.qty.units += order.qty.units;
        ++level.orders;
    }

    /// Unlink from the queue and decrement the level, erasing the level when
    /// it empties. A level left behind with zero orders would appear in the L2
    /// projection as a phantom price.
    void unlink(OrderRef ref) {
        Order& order = orders_[ref];
        auto& levels = (order.side == Side::kBid) ? bids_ : asks_;
        const auto it = levels.find(order.price.ticks);
        if (it == levels.end()) {
            return;
        }
        L3Level& level = it->second;

        if (order.prev != kNoOrder) {
            orders_[order.prev].next = order.next;
        } else {
            level.head = order.next;
        }
        if (order.next != kNoOrder) {
            orders_[order.next].prev = order.prev;
        } else {
            level.tail = order.prev;
        }
        order.prev = kNoOrder;
        order.next = kNoOrder;

        level.qty.units -= order.qty.units;
        --level.orders;
        if (level.orders == 0) {
            levels.erase(it);
        }
    }

    InstrumentSpec spec_;
    std::vector<Order> orders_;
    std::vector<OrderRef> free_list_;
    std::vector<Slot> table_;
    std::map<std::int64_t, L3Level> bids_;
    std::map<std::int64_t, L3Level> asks_;
    std::size_t live_orders_{0};
    std::size_t table_used_{0};
};

}  // namespace crossbook
