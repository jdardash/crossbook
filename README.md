# crossbook

**Multi-venue crypto order book reconstruction with continuous exchange-checksum verification.**

[![ci](https://github.com/jdardash/crossbook/actions/workflows/ci.yml/badge.svg)](https://github.com/jdardash/crossbook/actions/workflows/ci.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

A header-only C++20 library that rebuilds L2 order books from exchange
websocket feeds and **proves the result is correct against the exchange's own
arithmetic**, continuously, on live data.

Most order book implementations are tested against themselves. This one is
tested against Kraken's CRC32 checksum of the top 10 levels, published on every
single update — so "the book is correct" is a measurement with a number
attached, not an assertion.

---

## Why this is unusual

Crypto venues hand you a correctness oracle for free, and almost nobody collects
it:

| Venue | Ground truth | Public? |
|---|---|---|
| **Kraken** L2 `book` | CRC32 of the top 10 levels, on every update | Yes, no credentials |
| **Binance** spot diff-depth | `U`/`u` sequence bounds + REST snapshot reconciliation | Yes, no credentials |
| **Binance** futures diff-depth | `pu` continuity field | Yes, no credentials |
| **Coinbase** `full` | per-message `sequence` for gap detection | Requires auth |

In equities the equivalent data can't be redistributed, which is why every
public ITCH order book repo ships without runnable data. Here, `crossbook`'s
correctness claim is reproducible by anyone who clones it — no API key, no paid
feed, no sample file to trust.

## The detail that makes it cheap

Kraken computes its checksum by taking each level's price and quantity **as they
appear on the wire**, removing the decimal point, stripping leading zeros, and
concatenating. From their own documented example: price `45285.2` becomes
`452852`, quantity `0.00100000` becomes `100000`.

Both are exactly **the decimal digits of the value's fixed-point mantissa** at
the instrument's scale.

So storing prices as scaled `int64` mantissas — rather than `double` — turns
checksum generation into an integer-to-ASCII with no formatting step and no
rounding to get wrong. A `double`-based book physically cannot reproduce the
exchange's checksum without round-tripping through decimal formatting and
reintroducing the error it was trying to avoid.

Fixed-point here isn't a style preference. **The venue's own verification
algorithm requires it.** That's why the verifier runs on every update in the hot
path instead of being sampled.

The identity has one precondition — the venue must spell values canonically at
the instrument's scale — and that precondition is [documented, tested, and
guarded at ingest](include/crossbook/fixed.hpp) rather than assumed.

## Correctness, by five independent mechanisms

1. **Exchange checksum.** Kraken's CRC32, recomputed locally on every update.
2. **Differential testing.** Two independent book implementations — a `std::map`
   reference and a tick-indexed array — driven through identical event streams,
   with full state compared after *every single update*. Plus a
   [differential fuzzer](fuzz/fuzz_book.cpp) doing the same with
   coverage-guided adversarial input.
3. **Sequence continuity.** Per-venue gap detection with the correct contract
   for each. Spot and futures genuinely differ, and
   [a test asserts they disagree on the same event](tests/test_sequence.cpp).
4. **Determinism.** Identical input produces a bit-identical state hash on every
   platform. No floating point anywhere in the book, so there is nothing left
   that could vary by compiler or CPU. Checked across Linux, macOS, and Windows
   in CI.
5. **Recovery.** A book that is known to be wrong is never served and never
   updated further until it has been rebuilt from a snapshot. The failure that
   costs money is not the dropped message — it is applying the next one anyway.
   [`feed.hpp`](include/crossbook/feed.hpp) exists to make that impossible.

Divergences are never summarised away. Every mismatch is
[recorded with a cause](include/crossbook/divergence.hpp), because a match rate
without an enumerated remainder isn't evidence.

## Performance

Measured, with the methodology stated, because a number without one is noise.

**Book update — the path every message takes:**

| Spread from touch | `std::map` | tick-indexed array | speedup |
|---|---|---|---|
| 8 ticks (tight book) | 76.7 ns | **9.07 ns** | 8.5x |
| 200 ticks | 69.8 ns | **10.6 ns** | 6.6x |
| 5000 ticks (wide) | 136 ns | **11.2 ns** | 12.1x |

**Reads — where the array does *not* win:**

| Operation | `std::map` | tick-indexed array |
|---|---|---|
| best bid + best ask | **1.33 ns** | 2.67 ns |
| top 10 levels | **65.1 ns** | 72.1 ns |
| Kraken checksum (whole book) | 523 ns | 506 ns |

The array is decisively better on writes and roughly at parity or slightly
behind on reads. For a feed handler that is the right trade — every message is a
write, while reads happen once per decision — but it is not a clean sweep and is
not presented as one.

Checksum cost is independent of book depth (only 10 levels per side are ever
read) and allocation-free, so verifying every update costs well under a
microsecond.

**Methodology.** Median of 7 repetitions, Google Benchmark, MSVC 19.50 `/O2`,
Windows 11, 16 logical cores @ 2995 MHz. Standard deviation was under 5% of
median except where noted in the raw output. Reproduce with
`cmake --preset release && ./build/release/bench/crossbook_bench --benchmark_repetitions=7`.

**What these numbers are not.** These are *throughput* microbenchmarks — mean
time per operation, warm cache, tight loop. They are **not latency
measurements**. Nothing is pinned to a core, turbo and C-states are untouched,
and the input is synthetic. A mean is the wrong statistic for latency anyway,
because the distribution is heavily right-tailed and the mean hides the tail you
would actually be paid to fix.

Tail latency needs a different harness, and
[it now exists](include/crossbook/replay.hpp): `replay_open_loop` paces frames at
their recorded inter-arrival times regardless of whether the consumer keeps up,
and measures each one from the instant it was *supposed* to be processed rather
than the instant work began. A stall therefore lands on every message queued
behind it, exactly as production would experience — no correction step needed,
because nothing was omitted.

That distinction is not academic. A closed loop stops issuing work while it is
stalled, so a 100ms hiccup contributes one slow sample instead of the ten
thousand messages actually delayed behind it, and the reported p99.9 describes
the harness rather than the system. Gil Tene named this coordinated omission;
[a test](tests/test_replay.cpp) asserts the harness does not commit it.

No latency figures are published here yet. Producing them honestly needs a real
captured feed on tuned hardware — pinned cores, turbo and C-states disabled —
and until that exists, quoting numbers from a laptop would be exactly the kind
of unearned precision the rest of this README argues against.

### The benchmarks found a real bug

Worth stating plainly, because it is the reason the benchmarks exist.

The first implementation of the tick-indexed array scanned the window from its
edge to find the best price. Writes were 8x faster than the tree, exactly as
designed. Reads were **~15,000x slower** — 67 µs to answer "what is the best
bid", versus 4 ns for `std::map` — because every read walked ~32,000 empty slots.

Correct, fully passing its equivalence tests, and completely useless: every
quoting decision reads the touch. The fix was a maintained best-index hint, and
the differential oracle confirmed the optimisation changed no behaviour. Numbers
in the table above are post-fix; the commit history has both.

## Quick start

Requires CMake 3.24+ and a C++20 compiler (GCC 11+, Clang 14+, MSVC 19.30+).

```bash
git clone https://github.com/jdardash/crossbook.git
cd crossbook
cmake --preset release
cmake --build build/release
ctest --preset release
```

Header-only, so consuming it is just an include path:

```cmake
include(FetchContent)
FetchContent_Declare(crossbook
    GIT_REPOSITORY https://github.com/jdardash/crossbook.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(crossbook)
target_link_libraries(your_target PRIVATE crossbook::crossbook)
```

```cpp
#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

// BTC/USD on Kraken: 1 decimal of price, 8 of quantity.
ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});

// Parse wire values exactly — no double, ever.
const auto price = parse_fixed("45285.2", book.spec().price_scale);
const auto qty   = parse_fixed("0.00100000", book.spec().qty_scale);
book.apply(Side::kAsk, Price{price.mantissa}, Qty{qty.mantissa});

// Verify against what the exchange said.
if (kraken_checksum(book) != message_checksum) {
    // Enumerate it — never just count it.
    log.record({DivergenceKind::kChecksumMismatch, "kraken", "BTC/USD",
                ts, seq, message_checksum, kraken_checksum(book),
                kraken_checksum_payload(book)});
}
```

Or drive the whole pipeline — decode, verify, recover — from raw frames:

```cpp
#include "crossbook/feed.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;
using Kraken = Feed<venues::KrakenBookDecoder, ArrayBook>;

Kraken feed("kraken", venues::KrakenBookDecoder(InstrumentSpec{"BTC/USD", 1, 8}),
            SequencePolicy::kStrictIncrement);

for (std::string_view frame : frames_from_your_transport) {
    switch (feed.handle(frame)) {
        case FeedStatus::kApplied:       break;  // Verified against Kraken's CRC32.
        case FeedStatus::kIgnored:       break;  // Heartbeat, ack, stale duplicate.
        case FeedStatus::kRejected:      break;  // Logged; book untouched.
        case FeedStatus::kNeedsSnapshot:
            // The book is known wrong. Resubscribe. Do NOT read it until then.
            resubscribe();
            break;
    }
}

// feed.synced() must be true before anyone reads the book.
// feed.match_rate() is only evidence if feed.divergences().verified() > 0.
```

## Status

**v0.3 — the roadmap is done, except the socket.**

- [x] Exact fixed-point decimal, refuses to round rather than silently rounding
- [x] L2 book, two implementations, differentially tested against each other
- [x] Kraken CRC32 checksum, allocation-free, matching the documented algorithm
- [x] Sequence continuity for Binance spot, Binance futures, Coinbase
- [x] Divergence log with cause classification
- [x] No-allocation hot path, **enforced by a test** that hooks global `operator new`
- [x] Determinism via state hashing
- [x] Zero-dependency JSON scanner returning raw wire tokens
- [x] Kraken v2 `book` and Binance spot/futures depth decoders
- [x] Feed handler with resnapshot recovery and staleness detection
- [x] HDR histogram with coordinated-omission correction
- [x] Open-loop replay harness measuring against the schedule
- [x] L3 order-by-order book: arena-pooled intrusive queues, open-addressed
      id lookup, and queue position
- [x] `executable_size` and `cost_to_trade` — what you can actually trade
- [x] Consolidated cross-venue book: fee-adjusted, staleness-filtered
- [x] 196 test cases / 70k assertions, `-Werror`, ASan + UBSan, six fuzz targets
- [ ] Websocket transport — **deliberately not built.** Bring your own frames.

The library decodes, verifies, recovers, and prices execution; it does not open
sockets. That boundary is a choice, not an omission: TLS would end the
zero-dependency property that makes a header-only library adoptable, and socket
plumbing is both the least interesting part and the part every adopter already
has. Hand it frames from whatever transport you like.

## What you can actually trade

The touch is not a size. "Best ask 45283.6" says nothing about whether you can
buy one coin there or fifty, and sizing a position off it is the most common way
a spread that looked profitable turns out not to be.

```cpp
#include "crossbook/execution.hpp"

// How much can I buy within 5 bps of the touch?
const Execution e = executable_size(book, Side::kAsk, from_bps(5));
e.qty;              // Total available inside the limit
e.vwap;             // What you would actually pay, size-weighted
e.slippage;         // Cost against the touch, in 0.01 bps units
e.depth_exhausted;  // Ran out of book vs stopped by the limit — these differ

// What does one coin cost?
const Execution c = cost_to_trade(book, Side::kAsk, Qty{100'000'000});
```

`cost_to_trade` reports a partial fill rather than extrapolating a price for
size that is not in the book, because inventing depth is how a backtest produces
returns a live account cannot.

This is also the concrete reason a *correct* book matters rather than an
approximately correct one. The touch is refreshed constantly and self-corrects;
a level ten deep can sit there wrong for hours. Depth is exactly what a
reconstruction bug corrupts silently, and it is exactly what these functions
read.

## Cross-venue, without pretending there is an NBBO

Equities have Reg NMS, a SIP, and a legally defined national best bid and offer.
Crypto has none of it — no authority on the best price, no shared clock, no
obligation for venues to agree. `ConsolidatedBook` makes each judgement call
explicit instead of burying it:

- **Fees, not quotes.** Taker fees are large relative to crypto spreads, so the
  venue with the best headline price is frequently not the cheapest to trade.
  [A test](tests/test_consolidated.cpp) pins a case where a 26 bps venue quoting
  3 ticks better loses to a 1 bp venue.
- **Size changes the answer.** A venue can be best on one coin and worst on
  fifty. `best_execution` therefore takes a quantity and walks each venue's real
  depth — a size-free "best venue" is not a well-defined question.
- **Staleness excludes.** A quiet venue looks exactly like a stable one. Entries
  past a configured age are dropped rather than quoted.
- **Local clocks only.** Venue timestamps are never compared to each other.

## What this is not

- **Not a trading system.** No signals, no strategy, no positions, no PnL.
  There is no type in this library that represents a position.
- **Not a matching engine.** Exchanges do not need theirs rebuilt.
- **Not competitive with colocated production systems.** It is not tuned for a
  latency budget, and no benchmark here is run on tuned hardware.

It is a correctness-first feed handler core, built so that the claim "this book
is right" is something you can check rather than something you have to believe.

## Design notes

- [`fixed.hpp`](include/crossbook/fixed.hpp) — why prices are integers, and the
  one precondition the checksum fast path rests on
- [`book.hpp`](include/crossbook/book.hpp) — two side containers and why both stay
- [`checksum.hpp`](include/crossbook/checksum.hpp) — Kraken's algorithm, and why
  it is cheap here
- [`sequence.hpp`](include/crossbook/sequence.hpp) — three venue contracts, one
  state machine
- [`json.hpp`](include/crossbook/json.hpp) — why the scanner is hand-written, and
  why `well_formed` has to run first
- [`feed.hpp`](include/crossbook/feed.hpp) — the recovery state machine
- [`histogram.hpp`](include/crossbook/histogram.hpp) — HDR bucketing and
  coordinated-omission correction
- [`replay.hpp`](include/crossbook/replay.hpp) — open-loop pacing, and why the
  spin threshold is 20ms
- [`l3.hpp`](include/crossbook/l3.hpp) — arena, intrusive queues, and why a
  size reduction keeps priority while an increase does not
- [`execution.hpp`](include/crossbook/execution.hpp) — why the unit is 0.01 bps
- [`consolidated.hpp`](include/crossbook/consolidated.hpp) — the four judgement
  calls behind a crypto "best price"
- [`bench_book.cpp`](bench/bench_book.cpp) — what the numbers mean and don't

## References

- [Kraken WebSocket v2 book checksum](https://docs.kraken.com/api/docs/guides/spot-ws-book-v2)
- [Kraken `level3` channel](https://docs.kraken.com/api/docs/websocket-v2/level3)
- [Binance spot WebSocket streams](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams)
- [Binance futures: managing a local order book](https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams/How-to-manage-a-local-order-book-correctly)
- [Coinbase Exchange websocket channels](https://docs.cdp.coinbase.com/exchange/websocket-feed/channels)
- Gil Tene, *How NOT to Measure Latency* — the coordinated omission problem

## Contributing

Issues and PRs welcome. Two rules, both non-negotiable and both mechanically
checked:

1. **No floating point in the book.** Prices and quantities are integer
   mantissas. This is what makes checksums and determinism possible.
2. **No allocation on the hot path.** `tests/test_no_alloc.cpp` will fail the
   build if you add one.

## License

MIT — see [LICENSE](LICENSE).
