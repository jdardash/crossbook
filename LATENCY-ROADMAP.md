# Latency roadmap

Written 2026-08-02, from a four-track research pass: a file-level inventory of
this repository's hot path, and web research into wire formats, software
tick-to-trade practice, and tail-latency engineering. Claims below that came
from outside sources carry links; figures computed rather than measured are
marked as such. This document records where the gap to professional software
trading systems actually is, and the order in which to close it.

## Where the gap is

The reference point for a software (non-FPGA) trading system is roughly
2.5 us wire-to-wire ([Carl Cook, CppCon 2017](https://www.youtube.com/watch?v=NH1Tta7purM)).
Against that, this library's position splits three ways:

1. **The compute core is already in the right league.** ~8 ns book updates,
   ~600 ns checksum, ~2.1 us for a fully verified frame. Not the problem.
2. **The tail is the environment, not the code.** At 6k msg/s the handler sits
   at ~1.2% utilization; M/D/1 queueing puts the p99 physics floor around
   9-14 us. The measured 912 us p99 is ~100x that — Windows scheduler quanta,
   DPCs, C-state exits. A pinned busy-polling thread on an isolated core of
   tuned Linux measures p99 1.4 us / p99.9 4.9 us / max 18 us of induced
   jitter ([Rigtorp, hiccups](https://github.com/rigtorp/hiccups)); Windows
   has a hard ~100 us p99.9 floor from DPCs that no user-mode setting removes
   ([LatencyMon docs](https://resplendence.com/latencymon_using)).
3. **JSON is the structural ceiling on the median.** Decode is ~1.9 us, 75% of
   the frame. simdjson-class techniques put a validated ~300-byte frame at
   roughly 200-500 ns (computed from published GB/s throughput, not a bench),
   and binary venue feeds remove the cost entirely.

One calibration that bounds the whole effort: measured from AWS Tokyo,
Binance's own websocket delivery is avg 4 ms, p99 13 ms
([Deltix/Ember](https://ember.deltixlab.com/docs/performance/ws-market-data/)).
On crypto venues the exchange side is milliseconds; the competitive variable
is tail determinism, not median parse time.

## What the venues offer (2026)

| Venue | Fastest wire | Engine location |
|---|---|---|
| Kraken | FIX L3 (tag=value; no binary feed exists) | Equinix London; Beeks hosted colo |
| Binance spot | SBE WebSocket incl. L2 diff depth (Ed25519 key) | AWS Tokyo ap-northeast-1 |
| Deribit | SBE multicast, plaintext UDP; Starbase SBE L3 + order entry | Equinix LD4; AWS eu-west-2/ap-northeast-1 |
| Coinbase Exchange | FIX 5.0 L3 market data | AWS us-east-1 (use1-az4) |
| OKX / Bybit | JSON WebSocket only (public) | AWS HK / AWS Singapore |

Sources: [Kraken L3](https://docs.kraken.com/exchange/guides/general/l3-data),
[Binance SBE streams](https://developers.binance.com/docs/binance-spot-api-docs/sbe-market-data-streams),
[Deribit multicast](https://insights.deribit.com/exchange-updates/launch-of-our-new-multicast-service/),
[Deribit Starbase](https://insights.deribit.com/exchange-updates/starbase-a-new-era-of-high-performance-trading-on-deribit/),
[Coinbase FIX MD](https://docs.cdp.coinbase.com/exchange/fix-api/market-data).

## Phases

**Phase 0 — repository defects (done in the commit series that added this
document, except where noted).**

- `FrameReader::writable_tail` value-initialized 32 KiB per socket read
  (`vector::resize` zeroing), immediately shrunk back by `commit`. Same
  pattern on the Schannel ciphertext buffer.
- The Schannel decrypt path heap-allocated a fresh `std::vector` per
  pipelined TLS record and copied plaintext twice (OpenSSL path copies once).
- The no-allocation test enforced the book but not `Feed::handle` or the
  decoders — the 75% of the frame the claim was actually about.
- No opt-in `-march`/LTO configuration existed.

**Phase 1 — prove the tail on tuned Linux.** The repo already builds, tests,
and replays on Linux in CI, and `crossbook_verify` already carries `--pin`
and `--realtime`. On a box tuned per the standard recipe (isolcpus +
nohz_full + rcu_nocbs, IRQ affinity away, performance governor, C-states
capped at C1, SMT off, mlockall — [Rigtorp's guide](https://rigtorp.se/low-latency-guide/)),
qualified first with hwlatdetect and rtla osnoise, the existing `--sweep`
should collapse from 912 us p99 to tens of microseconds with zero code
changes. Publish the tuned-vs-untuned pair; it is the honest-measurement
story this README already tells, completed.

**Phase 2 — transport for latency.** The transport is a blocking `recv` with
a 1 s timeout and takes no timestamps. In order: a busy-poll read mode
(non-blocking socket, spin on an isolated core); `SO_TIMESTAMPING`
kernel/NIC receive timestamps threaded into the event, so measurement starts
at the wire rather than after recv + TLS + reassembly; and a revised
`Transport` read contract — the current copy-in `read(buf, len)` cannot
express zero-copy completion. Skip kTLS: RX-path p99 regressions
([netdev paper](https://netdevconf.info/1.2/papers/ktls.pdf)) and it blocks
the Onload route. Steady-state TLS crypto is under 1 us/record (computed from
~0.64 cycles/byte AES-GCM) and is not the problem.

**Phase 3 — a binary venue decoder.** Binance spot SBE is the only major
binary L2 diff-depth feed today and turns the ~1.9 us JSON decode into
struct-field reads; it also exercises the venue-decoder seam properly.
Deribit SBE multicast follows if derivatives matter — the only feed anywhere
that removes TLS entirely. Kraken's lever is placement plus FIX L3, not
encoding.

**Phase 4 — the JSON decode floor, for venues stuck with it.** Levers in
order: key dispatch by length/first byte instead of chained `string_view`
compares; SWAR digit parsing in `parse_fixed`; deriving canonical-spelling
during the parse instead of re-formatting and byte-comparing every scalar;
optionally a SIMD structural stage. Separately, the checksum's ~600 ns is
dominated by re-serializing 20 levels per message — maintain the top-10
payload incrementally as levels change instead.

**Phase 5 — placement and bypass.** In-region metal (c7i/c8g/m8azn) in a
shared cluster placement group measures ~20 us p50 / ~23 us p99.9
instance-to-instance ([AWS tick-to-trade series](https://aws.amazon.com/blogs/web3/optimize-tick-to-trade-latency-for-digital-assets-exchanges-and-trading-platforms-on-aws-part-2/));
exchanges pull market makers into their placement groups. Onload is the
drop-in kernel bypass for a TCP+TLS websocket client (sockets-compatible,
~6 us plus most network jitter); ef_vi/TCPDirect is a rewrite that buys the
last few hundred nanoseconds and comes last.

## Expected position

| Stage | p50/frame | p99 under load |
|---|---|---|
| Untuned Windows desktop (today) | ~2.1 us compute | 912 us |
| Phase 1: tuned Linux, same code | same | ~10-30 us |
| Phases 2-3: busy-poll + SBE venue | ~0.5-1 us | ~5-15 us |
| Phase 5: in-region metal + bypass | sub-us compute | ~20-25 us incl. cloud network |

The last row is competitive with the crypto-native trading tier. The
remaining distance to traditional-HFT numbers is the venues themselves,
which deliver data in milliseconds. Two standing caveats: several
per-technique figures above are computed or single-source, and nothing here
measures tick-to-trade until an order path exists — this repository has no
egress, so end-to-end latency is unmeasurable by construction.
