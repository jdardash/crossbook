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

Crypto venues hand you a correctness oracle for free, and the handlers that do
collect it treat it as optional. [cryptofeed](https://github.com/bmoscon/cryptofeed/blob/master/docs/book_validation.md)
validates across six venues behind a `checksum_validation=True` flag,
[NautilusTrader](https://nautilustrader.io/docs/latest/integrations/kraken/)
does Kraken L3 by default but needs credentials for it, and
[ccapi](https://github.com/crypto-chassis/ccapi) — the closest C++ analog —
implements checksums for OKX and Bitfinex, defaults them off, and has none for
Kraken at all. cryptofeed's own docs explain why they are opt-in: **10 to 100
microseconds per update.**

So the claim here is not that verification is novel. It is that verification
should not be a flag, and it only stops being one when it costs 506 ns instead
of 50 µs — which is a consequence of the fixed-point decision below, not of
being clever. What each venue hands you:

| Venue | Ground truth | Public? |
|---|---|---|
| **Kraken** L2 `book` | CRC32 of the top 10 levels, on every update | Yes, no credentials |
| **Binance** spot diff-depth | `U`/`u` sequence bounds + REST snapshot reconciliation | Yes, no credentials |
| **Binance** futures diff-depth | `pu` continuity field | Yes, no credentials |
| **Coinbase** `full` | per-message `sequence` for gap detection | Requires auth |

In equities the equivalent data can't be redistributed, which is why public ITCH
order book repositories generally ship without runnable data and ask to be
believed. Here, `crossbook`'s correctness claim is reproducible by anyone who
clones it — no API key, no paid feed, no sample file to trust. The committed
capture and the match rate it produces are in
[The measurement](#the-measurement) below.

## The detail that makes it cheap

Kraken computes its checksum by taking each level's price and quantity **as they
appear on the wire**, removing the decimal point, stripping leading zeros, and
concatenating. From their own documented example: price `45285.2` becomes
`452852`, quantity `0.00100000` becomes `100000`.

Both are exactly **the decimal digits of the value's fixed-point mantissa** at
the instrument's scale.

So storing prices as scaled `int64` mantissas — rather than `double` — turns
checksum generation into an integer-to-ASCII with no formatting step and no
rounding to get wrong. A `double`-based book has to format its way back to
decimal to reproduce the checksum: a step that costs more than the comparison it
enables, and that has to be exactly right at every instrument's scale. The
integer path does not have the step.

Fixed-point here isn't a style preference. **The venue's own verification
algorithm requires it.** That's why the verifier runs on every update in the hot
path instead of being sampled.

The identity has one precondition — the venue must spell values canonically at
the instrument's scale — and that precondition is [documented, tested, and
guarded at ingest](include/crossbook/fixed.hpp) rather than assumed.

## Correctness: one external oracle, four internal checks

The distinction matters, and calling all five "independent" would blur exactly
the point this README opens with. Only the first compares the book against
something outside this repository. The other four are consistency checks — they
are worth having, they catch real bugs, and they cannot tell you that your
reading of the venue's spec was wrong. That is the checksum's job, and it is why
the checksum is the one that runs on live data.

1. **Exchange checksum — the external oracle.** Kraken's CRC32, recomputed
   locally on every update. The only mechanism here that can find a bug in what
   we *believe* about Kraken, and
   [it did](#live-verification-found-a-real-bug).
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

## The measurement

`crossbook_verify` connects to Kraken, rebuilds the book, and recomputes the
exchange's CRC32 over local state on every update. No API key, no account.

```bash
cmake --preset release && cmake --build build/release
./build/release/tools/crossbook_verify --venue kraken --symbol BTC/USD --seconds 180
```

A three-minute run on BTC/USD, 2026-08-01:

```text
frames               2939
  applied            2759
checksums verified   2759
checksum mismatches  0
match rate           100.000000%  (2759 of 2759)
state hash           7648057f6909c67a
```

The exit status is the point: any mismatch, any decode failure, any resync and it
exits non-zero.

**And you can check this without taking my word for it.** A recorded minute of
that feed is committed at [`tests/fixtures/kraken_btcusd_l2.cbcap`](tests/fixtures/) — 72 KB
of verbatim Kraken bytes — and replays offline, deterministically, on every
platform:

```bash
./build/release/tools/crossbook_verify --replay tests/fixtures/kraken_btcusd_l2.cbcap
# 301 of 301 checksums matched, state hash e9613af632f40653
```

CI runs exactly that on Linux, macOS and Windows on every push, and asserts the
state hash is bit-identical across all three. That is why the number above is a
regression test rather than an anecdote. In equities the equivalent data is
licensed and cannot be redistributed, which is why every public ITCH order book
repository ships without runnable data and asks to be believed.

Recording your own is one command, and works for Binance too:

```bash
./build/release/tools/crossbook_capture --venue kraken --symbol ETH/USD \
    --seconds 60 --out eth.cbcap
./build/release/tools/crossbook_verify --replay eth.cbcap
```

`crossbook_capture` deliberately does not decode anything. Recording and
interpreting are separate jobs, and keeping them separate is what makes a
capture evidence rather than output: change the book implementation and the
capture is still the bytes the exchange sent, so the new implementation can be
held to them.

### Live verification found a real bug

Worth stating plainly, because it is the reason the verifier exists.

The first live run reported **98.66%** — 4 of 298 updates mismatched — and the
book held 20 bid levels for a subscription that asked for 10.

The cause is a gap in the depth-limited contract that unit tests do not reach.
Kraken reports cancellations, so a reader that handles those looks correct. It
never reports that a level fell out of the top ten because a *better* level
arrived — from the venue's side there is nothing to say. Those orphaned levels
sit below the checksummed depth doing no harm, until enough removals near the
touch promote one back into view, and then the checksum fails on an update that
was itself perfectly fine. The divergence is minutes away from its cause.

The fix is [`BasicL2Book::trim`](include/crossbook/book.hpp), and the reason it
is trustworthy is the same reason the bug was found: replaying the committed
capture with trimming disabled still fails, and
[a test asserts that it does](tests/test_fixture_replay.cpp).

No amount of testing the book against itself would have surfaced this. The
exchange's checksum did, in sixty seconds.

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

**What the 8.5x is actually for.** It is not the headline it looks like, and
pretending otherwise invites the obvious question. Across a whole Kraken frame:

| Stage | ns | share |
|---|---|---|
| JSON decode | ~960 | 65% |
| Kraken checksum | ~500 | 34% |
| book update | ~18 | **1.4%** |

So swapping the tick-indexed array back for a `std::map` would cost roughly
**9% end to end**, not 8.5x. The array's speedup is not what makes the library
fast — decode dominates, and that is where the remaining work is. What the
speedup buys is *headroom*: it is why verifying **every** update is affordable
instead of sampled, which is the trade this library exists to make and the one
the handlers cited at the top declined. A book update that cost 77 ns instead of
9 ns would not be slow; it would just make the 506 ns checksum harder to justify
on every message.

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
in the table above are post-fix.

That one happened before the first commit, so unlike the depth-trim bug above
you cannot dig it out of the history — take it as an anecdote about why the
benchmarks exist, not as evidence. The checksum bug is the one with the audit
trail: a capture you can replay, a fix you can diff, and a test that fails
without it.

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
    GIT_TAG v0.3.0
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(crossbook)
target_link_libraries(your_target PRIVATE crossbook::crossbook)
```

Or vendor it. There is no generated header, no configure step, and no
dependency outside the standard library, so copying the tree is a complete
install — and for a lot of desks that is the honest answer:

```bash
cp -r include/crossbook third_party/
```

`include/` is the entire library. `find_package(crossbook CONFIG REQUIRED)`
works too, against an installed prefix.

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
- [x] Websocket transport: RFC 6455 framing, TLS via Schannel and OpenSSL
- [x] Depth-limited book trimming — found by live verification, not by a test
- [x] Capture and byte-exact offline replay, with a recorded capture committed
- [x] `-Werror`, ASan + UBSan, and a differential fuzzer per subsystem
- [ ] Automatic Binance REST snapshot reconciliation in the tool (v0.3)

The correctness core still decodes, verifies, and recovers without opening a
socket: the transport is a separate, optional target, and consuming
`crossbook::crossbook` pulls in no TLS stack. `-DCROSSBOOK_BUILD_TOOLS=OFF`
drops it entirely. That boundary is what keeps the core testable offline —
which is also how the whole stack gets tested, since CI verifies a recorded
capture rather than a live venue.

### Dependencies, and the deliberate lack of them

The library has none: standard library only. The JSON reader, the RFC 6455
codec, SHA-1 and base64 are written here rather than pulled in, and two of those
are load-bearing rather than stylistic.

The JSON reader returns the **untouched wire token** for every value, because
Kraken's checksum is computed over the digits as the venue spelled them — a
parser that hands back a `double` has already destroyed the information needed
to verify the book. And SHA-1 is here so that `Sec-WebSocket-Accept` is actually
*verified* rather than assumed; that check is what proves the peer parsed the
upgrade request rather than merely answering 101, and it is the step most
hand-rolled clients skip.

TLS is the one thing that cannot reasonably be written here, so each platform's
own is used: Schannel on Windows, which ships with the OS, and OpenSSL
elsewhere. `cmake --build` therefore produces a working client on a stock
Windows machine with nothing installed.

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
- **Staleness excludes, and fails closed.** A quiet venue looks exactly like a
  stable one, so entries past a configured age are dropped rather than quoted.
  An *unstamped* quote is unusable rather than immortally fresh, and a
  timestamp in the future is treated as a clock fault — the failure modes here
  all had to be inverted, because every one of them originally failed open.
  `StalenessPolicy::kDisabled` is explicit rather than a magic zero.
- **Local clocks only.** Venue timestamps are never compared to each other.
- **Scales must match, and it is checked.** Two venues can quote the same
  instrument at different price and quantity scales, and comparing their raw
  mantissas is meaningless — Kraken at `price_scale=1` against Binance at `2`
  makes one venue win every bid and lose every ask, forever, while mismatched
  quantity scales silently corrupt the VWAP weights with no visible symptom.
  `VenueQuote` therefore carries its `InstrumentSpec`, `update()` returns
  `false` and refuses a quote that disagrees, and `best_execution` drops any
  book whose spec does not match.
- **Rounding costs the trader, never flatters them.** Integer division
  truncates toward zero, which understates what an ask costs and overstates
  what a bid pays — enough to invert the venue ranking outright. Fees round
  away from zero and VWAP rounds against the taker, so a venue is never
  reported cheaper than it is.
- **Ties break deterministically**, on venue name rather than arrival order.
  Two processes reading the same market must route identically, and rounding
  onto integer prices manufactures exact ties often enough for it to matter.

## Footprint, threading, and running many instruments

The questions a feed-handler engineer asks within a minute of reading
"tick-indexed array", answered plainly rather than left to be inferred.

**Memory.** `ArraySide` defaults to 65,536 slots of `int64`, so **512 KiB per
side and 1 MiB per `ArrayBook`, allocated up front whether the book is full or
empty.** At Kraken BTC/USD's 0.1 tick that window spans about $6.5k. Two
hundred instruments is therefore ~200 MiB of windows, which is the number to
budget against. `MapBook` has no floor and costs roughly 64 B per live level;
`Feed` is templated on the book type, so `Feed<Decoder, MapBook>` is a
one-word change when footprint matters more than update cost. The slot count is
a constructor parameter — 8,192 slots (64 KiB/side) is ample for a depth-10 or
depth-100 subscription and lets far more books stay resident in L2.

**Prices outside the window.** They are not dropped. They go to an overflow
container and stay correct there, and the window re-anchors around the touch
once enough have accumulated. That path is the one most likely to harbour a bug,
which is why the differential test hammers it — but it is also genuinely more
expensive than the array fast path, so a book whose *live span* persistently
exceeds the window is one that should be using `MapBook`.

**Threading.** The library is single-threaded and contains no `std::mutex`,
`std::atomic`, or `std::thread` by design. One `Feed` belongs to one thread; if
another thread reads the book, you supply the synchronisation. Nothing here
publishes a consistent snapshot for you, and `Feed::handle` is synchronous with
no internal queue, so backpressure between your socket and the handler is also
yours to design. This is a deliberate boundary — the same one that keeps the
correctness core testable offline — not an oversight, but it is your problem
and the README should say so.

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
