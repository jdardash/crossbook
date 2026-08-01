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
# 301 of 301 checksums matched, state hash 080281c2dd87183f
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

**v0.2 — decoders, recovery, and honest latency.**

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
- [x] Websocket transport: RFC 6455 framing, TLS via Schannel and OpenSSL
- [x] Depth-limited book trimming — found by live verification, not by a test
- [x] Capture and byte-exact offline replay, with a recorded capture committed
- [x] 196 test cases, `-Werror`, ASan + UBSan, five fuzz targets
- [ ] Automatic Binance REST snapshot reconciliation in the tool (v0.3)
- [ ] L3 / order-by-order books (v0.4)
- [ ] Cross-venue consolidated book and `executable_size` (v0.4)

The library still decodes, verifies, and recovers without opening a socket: the
transport is a separate, optional target, and consuming `crossbook::crossbook`
pulls in no TLS stack. `-DCROSSBOOK_BUILD_TOOLS=OFF` drops it entirely. That
boundary is what keeps the correctness core testable offline — which is also how
the whole stack gets tested, since CI verifies a recorded capture rather than a
live venue.

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
