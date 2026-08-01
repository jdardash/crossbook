// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Microbenchmarks for the book update path.
//
// READ THIS BEFORE QUOTING ANY NUMBER FROM HERE:
//
// These are THROUGHPUT microbenchmarks — mean time per operation under a tight
// loop with a warm cache. They answer "is the tick-indexed array actually
// faster than the tree, and by how much", which is the question that justifies
// ArraySide existing at all.
//
// They are NOT latency measurements. A mean is exactly the wrong statistic for
// latency, because the distribution is heavily right-tailed and the mean hides
// the tail you would actually be paid to fix. Nothing here is pinned to a core,
// nothing has turbo or C-states disabled, and the input is synthetic rather
// than a captured feed.
//
// Tail latency belongs to the open-loop replay harness (v0.2), which paces
// messages at their recorded inter-arrival times regardless of whether the
// consumer keeps up, and reports a full HdrHistogram. Measuring latency with a
// closed loop like this one produces numbers that are optimistic by orders of
// magnitude — the coordinated omission problem — and the fix is a different
// harness, not a different statistic over this one.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

namespace {

struct Update {
    Side side;
    std::int64_t price;
    std::int64_t qty;
};

/// Updates clustered around a drifting touch, which is what a real book does.
/// A uniform spread over the whole price range would never exercise the window
/// and would flatter the tree.
std::vector<Update> make_stream(std::size_t count, std::int64_t spread, std::uint32_t seed = 7) {
    std::mt19937 rng(seed);
    std::vector<Update> out;
    out.reserve(count);
    std::int64_t touch = 452852;
    for (std::size_t i = 0; i < count; ++i) {
        touch += static_cast<std::int64_t>(rng() % 5) - 2;
        const Side side = (rng() % 2) ? Side::kBid : Side::kAsk;
        const auto offset = static_cast<std::int64_t>(rng() % static_cast<std::uint32_t>(spread));
        const std::int64_t price = (side == Side::kBid) ? touch - offset : touch + 1 + offset;
        const std::int64_t qty =
            (rng() % 4 == 0) ? 0 : static_cast<std::int64_t>(rng() % 1'000'000) + 1;
        out.push_back(Update{side, price, qty});
    }
    return out;
}

template <typename BookT>
BookT warm_book(std::int64_t depth) {
    BookT book(InstrumentSpec{"BTC/USD", 1, 8});
    for (std::int64_t i = 0; i < depth; ++i) {
        book.apply(Side::kBid, Price{452852 - i}, Qty{100000 + i});
        book.apply(Side::kAsk, Price{452853 + i}, Qty{100000 + i});
    }
    return book;
}

}  // namespace

// ---------------------------------------------------------------------------
// Update throughput: the headline comparison
// ---------------------------------------------------------------------------

template <typename BookT>
void BM_Apply(benchmark::State& state) {
    const auto spread = static_cast<std::int64_t>(state.range(0));
    const auto stream = make_stream(65536, spread);

    BookT book(InstrumentSpec{"BTC/USD", 1, 8});
    // Pre-warm so the measurement covers steady state, not first-touch page
    // faults on the window buffer.
    for (const Update& u : stream) {
        book.apply(u.side, Price{u.price}, Qty{u.qty});
    }

    std::size_t i = 0;
    for (auto _ : state) {
        const Update& u = stream[i++ & (stream.size() - 1)];
        book.apply(u.side, Price{u.price}, Qty{u.qty});
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["spread_ticks"] = static_cast<double>(spread);
}

// Spread controls how far from the touch updates land: 8 is a tight book,
// 5000 is a wide one where the window has to work harder.
BENCHMARK_TEMPLATE(BM_Apply, MapBook)->Arg(8)->Arg(200)->Arg(5000);
BENCHMARK_TEMPLATE(BM_Apply, ArrayBook)->Arg(8)->Arg(200)->Arg(5000);

// ---------------------------------------------------------------------------
// Touch lookup: what a quoting strategy reads on every single update
// ---------------------------------------------------------------------------

template <typename BookT>
void BM_BestBidAsk(benchmark::State& state) {
    auto book = warm_book<BookT>(state.range(0));
    Level lvl{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.best(Side::kBid, lvl));
        benchmark::DoNotOptimize(book.best(Side::kAsk, lvl));
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

BENCHMARK_TEMPLATE(BM_BestBidAsk, MapBook)->Arg(10)->Arg(500);
BENCHMARK_TEMPLATE(BM_BestBidAsk, ArrayBook)->Arg(10)->Arg(500);

// ---------------------------------------------------------------------------
// Checksum: the cost of verifying every update instead of sampling
// ---------------------------------------------------------------------------

template <typename BookT>
void BM_KrakenChecksum(benchmark::State& state) {
    auto book = warm_book<BookT>(state.range(0));
    for (auto _ : state) {
        benchmark::DoNotOptimize(kraken_checksum(book));
    }
    state.SetItemsProcessed(state.iterations());
}

// Depth barely matters: the checksum only ever reads 10 levels per side. Both
// depths are measured to demonstrate exactly that.
BENCHMARK_TEMPLATE(BM_KrakenChecksum, MapBook)->Arg(10)->Arg(500);
BENCHMARK_TEMPLATE(BM_KrakenChecksum, ArrayBook)->Arg(10)->Arg(500);

/// The allocating debug path, for contrast. Only used when reporting a
/// divergence, never in the verify loop — this benchmark is why.
void BM_KrakenChecksumPayload(benchmark::State& state) {
    auto book = warm_book<ArrayBook>(50);
    for (auto _ : state) {
        benchmark::DoNotOptimize(kraken_checksum_payload(book));
    }
}
BENCHMARK(BM_KrakenChecksumPayload);

// ---------------------------------------------------------------------------
// Top-of-book snapshot
// ---------------------------------------------------------------------------

template <typename BookT>
void BM_TopTen(benchmark::State& state) {
    auto book = warm_book<BookT>(500);
    std::vector<Level> out;
    out.reserve(10);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.top(Side::kBid, 10, out));
    }
}

BENCHMARK_TEMPLATE(BM_TopTen, MapBook);
BENCHMARK_TEMPLATE(BM_TopTen, ArrayBook);

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

void BM_ParseFixed(benchmark::State& state) {
    // Realistic wire tokens at Kraken's BTC/USD scales.
    const char* tokens[] = {"45285.2", "45285.3", "0.00100000", "12.34567890", "999999.9"};
    const Scale scales[] = {1, 1, 8, 8, 1};
    std::size_t i = 0;
    for (auto _ : state) {
        const std::size_t k = i++ % 5;
        benchmark::DoNotOptimize(parse_fixed(tokens[k], scales[k]).mantissa);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseFixed);

BENCHMARK_MAIN();
