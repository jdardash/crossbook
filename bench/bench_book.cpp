// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Microbenchmarks for the message path.
//
// READ THIS BEFORE QUOTING ANY NUMBER FROM HERE:
//
// These are THROUGHPUT microbenchmarks — mean time per operation under a tight
// loop with a warm cache. Nothing here is pinned to a core, nothing has turbo
// or C-states disabled, and the input is synthetic rather than a captured feed.
//
// They are NOT latency measurements. A mean is exactly the wrong statistic for
// latency, because the distribution is heavily right-tailed and the mean hides
// the tail you would actually be paid to fix. Tail latency belongs to the
// open-loop replay harness (v0.2), which paces messages at their recorded
// inter-arrival times regardless of whether the consumer keeps up, and reports
// a full HdrHistogram. Measuring latency with a closed loop like this one
// produces numbers that are optimistic by orders of magnitude — the coordinated
// omission problem — and the fix is a different harness, not a different
// statistic over this one.
//
// WHAT THIS FILE USED TO MEASURE, AND WHY THAT WAS MISLEADING:
//
// It benchmarked ArraySide::apply against MapSide::apply, and nothing else,
// which made the container choice look like the whole story. It is not. One
// Kraken update end to end splits roughly:
//
//     JSON decode       ~960 ns   65%
//     kraken_checksum   ~500 ns   34%
//     ArraySide::apply  ~16-21 ns  1.4%
//
// So swapping the tick-indexed array for std::map moves about 8.6% of the
// end-to-end cost, not the 8.5x that a benchmark of the 1.4% slice implies. A
// reader who saw only the apply row would go and optimise the one component
// that cannot pay for itself. BM_KrakenDecode and BM_FeedHandle exist so that
// ratio is measured rather than asserted, and BM_ApplyThenChecksum exists
// because apply-then-verify is literally the Kraken path: benchmarking a
// checksum over a frozen book measures a warmer machine than production offers,
// because in production the write immediately precedes the read.
//
// Three further things this file now does that it did not:
//
//   - It parameterises the book by TICK GAP. warm_book used to build a
//     perfectly dense book — consecutive ticks, no holes — and ArraySide is at
//     its theoretical best there because for_each never steps over an empty
//     slot. Real Kraken BTC/USD L2 at a 0.1 tick publishes a level every $0.50
//     to $10, which is 5 to 100 empty slots between neighbours. Publishing only
//     the dense row publishes the array's best case as if it were its typical
//     one, and the gap is where the array loses.
//
//   - It reports overflow_size() on every book benchmark. That accessor exists
//     "so benchmarks and tests can assert the window is actually doing its job
//     rather than silently degrading into a std::map with extra steps", and no
//     benchmark read it. From the timings alone, a window that has quietly
//     stopped working and a window that is working look the same until the
//     numbers are already published.
//
//   - It includes configurations that deliberately defeat the window, so the
//     degraded path has a published number instead of being a case the design
//     note asserts cannot happen.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"
#include "crossbook/feed.hpp"
#include "crossbook/fixed.hpp"
#include "crossbook/venue.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;
using namespace crossbook::venues;

namespace {

/// Kraken BTC/USD: price scale 1 (a 0.1 tick), quantity scale 8.
const InstrumentSpec& spec() {
    static const InstrumentSpec s{"BTC/USD", 1, 8};
    return s;
}

/// A plausible touch for BTC/USD in mantissa form: 45285.2.
constexpr std::int64_t kTouch = 452852;

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
    std::int64_t touch = kTouch;
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

/// A book of `depth` levels per side, spaced `gap` ticks apart.
///
/// THE GAP PARAMETER IS THE POINT. At gap == 1 the book is perfectly dense and
/// ArraySide::for_each never steps over an empty slot; every level it wants is
/// the next one along. At gap == 100 it steps over 99 empty slots per level,
/// and every read that walks levels — the checksum, top(N), state_hash — pays
/// for all of them. std::map does not care about the gap at all, because it
/// stores levels rather than ticks. That asymmetry is the array's real cost and
/// it was previously unmeasured.
template <typename BookT>
BookT warm_book(std::int64_t depth, std::int64_t gap = 1) {
    BookT book(spec());
    for (std::int64_t i = 0; i < depth; ++i) {
        book.apply(Side::kBid, Price{kTouch - i * gap}, Qty{100000 + i});
        book.apply(Side::kAsk, Price{kTouch + 1 + i * gap}, Qty{100000 + i});
    }
    return book;
}

// ---------------------------------------------------------------------------
// Counters: what the timings alone will not tell you
// ---------------------------------------------------------------------------

/// MapSide has no window and therefore nothing to spill. Reported as zero
/// rather than omitted, so the column lines up across both implementations and
/// a missing number is never mistaken for a zero one.
void report_book_counters(benchmark::State& state, const MapBook& book) {
    state.counters["levels"] = static_cast<double>(book.bids().size() + book.asks().size());
    state.counters["overflow"] = 0.0;
}

/// The number that says whether ArraySide is still an array.
///
/// Every level in overflow_ is a level being served out of a std::map, by an
/// implementation whose entire justification is not being a std::map. A run
/// that reports a large overflow is reporting a std::map with a 512 KiB buffer
/// attached, and the timing on that row means something completely different
/// from the timing on a row that reports zero.
void report_book_counters(benchmark::State& state, const ArrayBook& book) {
    state.counters["levels"] = static_cast<double>(book.bids().size() + book.asks().size());
    state.counters["overflow"] =
        static_cast<double>(book.bids().overflow_size() + book.asks().overflow_size());
}

// ---------------------------------------------------------------------------
// Kraken frame generation
//
// The decode and feed benchmarks need real frames, not a fixed string: a decode
// benchmark over a hardcoded three-level message measures the fixed cost of
// finding four keys and nothing about the per-level work that dominates a real
// snapshot. Frames are rendered from mantissas through format_fixed, so the
// tokens on the wire are exactly what parse_fixed has to reverse.
// ---------------------------------------------------------------------------

std::string render_frame(std::string_view type, const std::vector<LevelUpdate>& levels,
                         std::uint32_t checksum) {
    std::string out;
    out.reserve(levels.size() * 48 + 128);
    out += R"({"channel":"book","type":")";
    out += type;
    out += R"(","data":[{"symbol":"BTC/USD",)";

    for (const Side side : {Side::kBid, Side::kAsk}) {
        out += (side == Side::kBid) ? R"("bids":[)" : R"("asks":[)";
        bool first = true;
        for (const LevelUpdate& lvl : levels) {
            if (lvl.side != side) {
                continue;
            }
            if (!first) {
                out += ',';
            }
            first = false;
            out += R"({"price":)";
            out += format_fixed(lvl.price.ticks, spec().price_scale);
            out += R"(,"qty":)";
            out += format_fixed(lvl.qty.units, spec().qty_scale);
            out += '}';
        }
        out += "],";
    }

    out += R"("checksum":)";
    out += std::to_string(checksum);
    // Kraken sends a timestamp the decoder never reads. Included because the
    // scanner still has to walk past it to reach the keys it does read, and a
    // decode benchmark over a frame with the trailing fields stripped is
    // measuring a message no venue sends.
    out += R"(,"timestamp":"2026-07-31T12:00:00.000000Z"}]})";
    return out;
}

/// A self-consistent cycle of Kraken frames: one snapshot followed by
/// `update_count` updates, each carrying the checksum the exchange would have
/// published for the book state that update produces.
///
/// WHY A CYCLE RATHER THAN A LIST. A feed benchmark has to replay its input
/// forever, and a book fed the same update twice does not end up in the same
/// state, so the second lap's checksums would all fail and the benchmark would
/// silently be measuring a desynced feed rejecting frames. Leading with a
/// snapshot fixes that: a snapshot is a replacement, so lap N+1 starts from
/// exactly the state lap N started from, and verification stays green forever.
/// BM_FeedHandle asserts that it did.
struct KrakenCycle {
    std::vector<std::string> frames;  ///< frames[0] is the snapshot.
    std::size_t bytes{0};
};

KrakenCycle make_kraken_cycle(std::size_t snapshot_levels, std::size_t update_count,
                              std::size_t update_levels, std::int64_t gap) {
    KrakenCycle cycle;
    cycle.frames.reserve(update_count + 1);

    ArrayBook shadow(spec());
    std::vector<LevelUpdate> levels;

    levels.clear();
    for (std::size_t i = 0; i < snapshot_levels; ++i) {
        const auto k = static_cast<std::int64_t>(i);
        levels.push_back(LevelUpdate{Side::kBid, Price{kTouch - k * gap}, Qty{100000 + k}});
        levels.push_back(LevelUpdate{Side::kAsk, Price{kTouch + 1 + k * gap}, Qty{100000 + k}});
    }
    shadow.clear();
    for (const LevelUpdate& lvl : levels) {
        shadow.apply(lvl.side, lvl.price, lvl.qty);
    }
    cycle.frames.push_back(render_frame("snapshot", levels, kraken_checksum(shadow)));

    std::mt19937 rng(11);
    for (std::size_t u = 0; u < update_count; ++u) {
        levels.clear();
        for (std::size_t j = 0; j < update_levels; ++j) {
            const Side side = (rng() % 2) ? Side::kBid : Side::kAsk;
            // Churn lands in the top ten, which is where a real book churns and
            // also the only region the checksum covers. An update outside it
            // would leave the checksum unchanged and make the verification half
            // of this measurement free.
            const auto rank = static_cast<std::int64_t>(rng() % 10);
            const std::int64_t price =
                (side == Side::kBid) ? kTouch - rank * gap : kTouch + 1 + rank * gap;
            // Never zero: a deletion stream slowly empties the top of the book,
            // and by the end of the cycle the benchmark would be checksumming a
            // two-level book while claiming to checksum a five-hundred-level
            // one. Quantity changes keep the shape fixed and the cost honest.
            const std::int64_t qty = static_cast<std::int64_t>(rng() % 1'000'000) + 1;
            levels.push_back(LevelUpdate{side, Price{price}, Qty{qty}});
        }
        for (const LevelUpdate& lvl : levels) {
            shadow.apply(lvl.side, lvl.price, lvl.qty);
        }
        cycle.frames.push_back(render_frame("update", levels, kraken_checksum(shadow)));
    }

    for (const std::string& f : cycle.frames) {
        cycle.bytes += f.size();
    }
    return cycle;
}

}  // namespace

// ---------------------------------------------------------------------------
// The real hot path: decode, apply, verify
//
// Everything below this line was missing, and it is the part that decides
// whether any of the container work was worth doing.
// ---------------------------------------------------------------------------

/// JSON decode alone — the 65% slice.
///
/// Parameterised by levels per side, because the per-level cost (two find()
/// calls and two parse_fixed calls per level) is what actually scales, and a
/// single fixed-size frame cannot show that.
void BM_KrakenDecode(benchmark::State& state) {
    const auto levels_per_side = static_cast<std::size_t>(state.range(0));
    const KrakenCycle cycle = make_kraken_cycle(levels_per_side, 0, 0, 5);
    const std::string& frame = cycle.frames.front();

    KrakenBookDecoder decoder(spec());
    if (!decoder.decode(frame).ok()) {
        state.SkipWithError("generated frame does not decode; the benchmark would be timing "
                            "an early return rather than a decode");
        return;
    }

    for (auto _ : state) {
        const DecodedMessage& msg = decoder.decode(frame);
        benchmark::DoNotOptimize(msg.levels.data());
        benchmark::DoNotOptimize(msg.checksum);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * frame.size()));
    state.counters["levels"] = static_cast<double>(levels_per_side * 2);
    state.counters["frame_bytes"] = static_cast<double>(frame.size());
}

// 1 level is a typical incremental update; 500 is a snapshot. The gap between
// them is the per-level cost, which is the number worth having.
BENCHMARK(BM_KrakenDecode)->Arg(1)->Arg(10)->Arg(100)->Arg(500);

/// The whole thing: decode, apply every level, trim, and verify against the
/// exchange's CRC32. This is what a frame off the socket actually costs, and it
/// is the denominator every other number in this file should be read against.
template <typename BookT>
void BM_FeedHandle(benchmark::State& state) {
    const auto update_levels = static_cast<std::size_t>(state.range(0));
    // A depth-100 subscription with 1023 updates between snapshots. The ratio
    // is deliberate: the cycle has to contain a snapshot to be replayable, and
    // a snapshot is two orders of magnitude more expensive than an update, so a
    // short cycle would report a number that is mostly snapshot. At 1 in 1024 a
    // 100-level snapshot contributes about 4% of the reported mean, and
    // `frames_per_snapshot` is published so the correction can be applied.
    const KrakenCycle cycle = make_kraken_cycle(100, 1023, update_levels, 5);

    Feed<KrakenBookDecoder, BookT> feed("kraken", KrakenBookDecoder(spec()),
                                        SequencePolicy::kStrictIncrement, 0);

    std::size_t i = 0;
    for (auto _ : state) {
        const FeedStatus status = feed.handle(cycle.frames[i]);
        benchmark::DoNotOptimize(status);
        if (++i == cycle.frames.size()) {
            i = 0;
        }
    }

    // A feed that lost sync rejects frames on a much cheaper path, so a silent
    // desync would show up as an impressively fast benchmark. Fail loudly
    // instead: an unverified number here is worse than no number.
    if (feed.stats().checksum_mismatches != 0 || !feed.synced()) {
        state.SkipWithError("feed lost sync during the benchmark; the timing is measuring "
                            "rejection, not the hot path");
        return;
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(static_cast<std::int64_t>(
        state.iterations() * cycle.bytes / cycle.frames.size()));
    state.counters["update_levels"] = static_cast<double>(update_levels);
    state.counters["frames_per_snapshot"] = static_cast<double>(cycle.frames.size());
    state.counters["verified"] = static_cast<double>(feed.stats().checksums_verified);
    state.counters["mismatches"] = static_cast<double>(feed.stats().checksum_mismatches);
    report_book_counters(state, feed.book());
}

// One level per update is the common Kraken update; 10 and 50 cover bursts. One
// frame in 257 is the cycle's snapshot, so the mean carries about 0.4% of a
// 500-level snapshot in it.
BENCHMARK_TEMPLATE(BM_FeedHandle, MapBook)->Arg(1)->Arg(10)->Arg(50);
BENCHMARK_TEMPLATE(BM_FeedHandle, ArrayBook)->Arg(1)->Arg(10)->Arg(50);

/// One apply immediately followed by one checksum — the Kraken verify loop.
///
/// Measured separately from BM_KrakenChecksum because that one checksums a book
/// nothing is writing to, so the top-of-book slots stay resident and clean in
/// L1 across every iteration. In production the checksum reads memory the apply
/// just dirtied. This benchmark pays that cost; the frozen-book one does not.
template <typename BookT>
void BM_ApplyThenChecksum(benchmark::State& state) {
    const auto depth = state.range(0);
    const auto gap = state.range(1);
    auto book = warm_book<BookT>(depth, gap);

    // Quantity changes at existing top-of-book prices: the book's shape stays
    // fixed, so every iteration checksums the same number of levels and the
    // measurement is not quietly a measurement of the book shrinking.
    std::vector<Update> churn;
    churn.reserve(64);
    std::mt19937 rng(23);
    for (std::size_t i = 0; i < 64; ++i) {
        const Side side = (rng() % 2) ? Side::kBid : Side::kAsk;
        const auto rank = static_cast<std::int64_t>(rng() % 10);
        const std::int64_t price =
            (side == Side::kBid) ? kTouch - rank * gap : kTouch + 1 + rank * gap;
        churn.push_back(
            Update{side, price, static_cast<std::int64_t>(rng() % 1'000'000) + 1});
    }

    std::size_t i = 0;
    for (auto _ : state) {
        const Update& u = churn[i++ & (churn.size() - 1)];
        book.apply(u.side, Price{u.price}, Qty{u.qty});
        benchmark::DoNotOptimize(kraken_checksum(book));
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["gap_ticks"] = static_cast<double>(gap);
    report_book_counters(state, book);
}

BENCHMARK_TEMPLATE(BM_ApplyThenChecksum, MapBook)
    ->Args({500, 1})
    ->Args({500, 20})
    ->Args({100, 100})
    ->Args({500, 100});
BENCHMARK_TEMPLATE(BM_ApplyThenChecksum, ArrayBook)
    ->Args({500, 1})
    ->Args({500, 20})
    ->Args({100, 100})
    ->Args({500, 100});

// ---------------------------------------------------------------------------
// Update throughput: the original comparison, now with the window instrumented
// ---------------------------------------------------------------------------

template <typename BookT>
void BM_Apply(benchmark::State& state) {
    const auto spread = static_cast<std::int64_t>(state.range(0));
    const auto stream = make_stream(65536, spread);

    BookT book(spec());
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
    report_book_counters(state, book);
}

// Spread controls how far from the touch updates land: 8 is a tight book, 5000
// is a wide one.
//
// 40000 IS THE ONE THAT MATTERS AND IT DID NOT EXIST. The window is 65536 slots
// centred on the touch, so nothing lands outside it until updates reach ±32768
// ticks; with the old maximum of 5000 and a touch that random-walks about 360
// ticks, every single update landed in the window and overflow_ was never
// touched. rebuild_around_touch — the code the design notes call "the part most
// likely to harbour a bug" — had no benchmark at all. At 40000 roughly a fifth
// of updates spill, the overflow counter goes non-zero, and the rebuild path
// runs. Read the `overflow` column on this row before reading the time.
BENCHMARK_TEMPLATE(BM_Apply, MapBook)->Arg(8)->Arg(200)->Arg(5000)->Arg(40000);
BENCHMARK_TEMPLATE(BM_Apply, ArrayBook)->Arg(8)->Arg(200)->Arg(5000)->Arg(40000);

// ---------------------------------------------------------------------------
// Touch lookup: what a quoting strategy reads on every single update
// ---------------------------------------------------------------------------

/// WHAT WAS WRONG HERE, BECAUSE IT IS INSTRUCTIVE:
///
/// This loop used to read
///
///     benchmark::DoNotOptimize(book.best(Side::kBid, lvl));
///
/// which sinks the returned BOOL and says nothing about `lvl`. best() is fully
/// inlinable, `book` is never written to inside the loop, and `lvl` was not
/// observed — so the compiler was free to compute the answer once, hoist it,
/// and leave the loop doing very little. A direct probe of the same call
/// measured 1.9 ns while this benchmark reported ~3.9 ns per call, which means
/// the published "map 1.33 ns vs array 2.67 ns" row — the table's one conceded
/// loss, and therefore the row a sceptical reader trusts most — was measuring
/// something other than what it claimed.
///
/// The fix: name the result, observe BOTH the Level and the bool, and clobber
/// memory so the compiler cannot assume `book` is unchanged between iterations.
template <typename BookT>
void BM_BestBidAsk(benchmark::State& state) {
    auto book = warm_book<BookT>(state.range(0), state.range(1));
    Level bid{};
    Level ask{};
    for (auto _ : state) {
        const bool have_bid = book.best(Side::kBid, bid);
        const bool have_ask = book.best(Side::kAsk, ask);
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
        benchmark::DoNotOptimize(have_bid);
        benchmark::DoNotOptimize(have_ask);
        benchmark::ClobberMemory();
    }
    // TWO best() CALLS PER ITERATION. The Time column is therefore per pair,
    // not per call; items_per_second is the per-call figure. Quoting the Time
    // column as ns-per-call doubles it, which is half of how the old number got
    // to ~3.9 ns.
    state.SetItemsProcessed(state.iterations() * 2);
    state.counters["gap_ticks"] = static_cast<double>(state.range(1));
    state.counters["calls_per_iter"] = 2.0;
    report_book_counters(state, book);
}

BENCHMARK_TEMPLATE(BM_BestBidAsk, MapBook)
    ->Args({10, 1})
    ->Args({500, 1})
    ->Args({100, 100})
    ->Args({500, 100});
BENCHMARK_TEMPLATE(BM_BestBidAsk, ArrayBook)
    ->Args({10, 1})
    ->Args({500, 1})
    ->Args({100, 100})
    ->Args({500, 100});

// ---------------------------------------------------------------------------
// Checksum: the cost of verifying every update instead of sampling
// ---------------------------------------------------------------------------

/// Depth barely matters — the checksum only ever reads 10 levels per side — but
/// the GAP matters enormously, and only the dense row was ever published.
/// ArraySide::for_each has to walk the empty ticks between levels; MapSide does
/// not, because it stores levels rather than ticks. At a realistic gap the
/// tick-indexed array is the slower of the two here.
template <typename BookT>
void BM_KrakenChecksum(benchmark::State& state) {
    auto book = warm_book<BookT>(state.range(0), state.range(1));
    for (auto _ : state) {
        benchmark::DoNotOptimize(kraken_checksum(book));
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["gap_ticks"] = static_cast<double>(state.range(1));
    report_book_counters(state, book);
}

// TWO SEPARATE DEGRADATIONS LIVE IN THESE ROWS, AND CONFLATING THEM WOULD BE
// THE SAME MISTAKE THIS FILE IS FIXING:
//
//   {100, 100} — 100 levels at a 100-tick gap spans 10k ticks, comfortably
//                inside the 65536-slot window, so `overflow` is 0. Any slowdown
//                here is purely for_each stepping over empty slots.
//   {500, 100} — 500 levels at a 100-tick gap spans 50k ticks per side, which
//                does NOT fit in a window centred on the touch. Part of the
//                book is being served out of the overflow std::map, which the
//                `overflow` column reports. That row is a different experiment
//                and must not be quoted as the gap cost.
BENCHMARK_TEMPLATE(BM_KrakenChecksum, MapBook)
    ->Args({10, 1})
    ->Args({100, 1})
    ->Args({100, 100})
    ->Args({500, 1})
    ->Args({500, 20})
    ->Args({500, 100});
BENCHMARK_TEMPLATE(BM_KrakenChecksum, ArrayBook)
    ->Args({10, 1})
    ->Args({100, 1})
    ->Args({100, 100})
    ->Args({500, 1})
    ->Args({500, 20})
    ->Args({500, 100});

/// The allocating debug path, for contrast. Only used when reporting a
/// divergence, never in the verify loop — this benchmark is why.
void BM_KrakenChecksumPayload(benchmark::State& state) {
    auto book = warm_book<ArrayBook>(50, 1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(kraken_checksum_payload(book));
    }
    report_book_counters(state, book);
}
BENCHMARK(BM_KrakenChecksumPayload);

// ---------------------------------------------------------------------------
// Top-of-book snapshot
// ---------------------------------------------------------------------------

/// Reading the top ten is what every consumer of this book does, and it is the
/// read the gap hurts most: ArraySide::for_each has to cross `gap - 1` empty
/// slots between each of the ten levels it wants, while MapSide takes ten
/// steps regardless. The dense row was the only one ever published.
template <typename BookT>
void BM_TopTen(benchmark::State& state) {
    auto book = warm_book<BookT>(state.range(0), state.range(1));
    std::vector<Level> out;
    out.reserve(10);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.top(Side::kBid, 10, out));
        benchmark::DoNotOptimize(out.data());
    }
    state.counters["gap_ticks"] = static_cast<double>(state.range(1));
    report_book_counters(state, book);
}

// As with the checksum: {100, 100} isolates the empty-slot scan with the window
// intact, {500, 100} additionally overflows it. Read the `overflow` column.
BENCHMARK_TEMPLATE(BM_TopTen, MapBook)
    ->Args({500, 1})
    ->Args({500, 20})
    ->Args({100, 100})
    ->Args({500, 100});
BENCHMARK_TEMPLATE(BM_TopTen, ArrayBook)
    ->Args({500, 1})
    ->Args({500, 20})
    ->Args({100, 100})
    ->Args({500, 100});

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
