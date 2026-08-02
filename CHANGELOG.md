# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) with the usual 0.x
caveat: while the major version is 0, a **minor** bump is where breaking
changes live.

This file exists because the documented way to consume crossbook is to pin a
tag. A reader who pins `v0.1.0` and later wants `v0.3.0` had, until now, no way
to find out what moved between them short of reading the diff — and the diff is
where the answer that "`crossbook/feed.hpp` did not exist at `v0.1.0`" was
hiding while the README's own example told people to pin it.

**Breaking changes are listed first in every section**, because they are the
only entries that can cost a reader an afternoon.

## [Unreleased]

### Added

- `net::ByteBuffer`: a `std::vector<char>` whose `resize` default-initializes
  instead of zeroing. The frame reader and the Schannel backend grow their
  receive buffers by a 32 KiB chunk on every socket read and trim back to what
  arrived; with a plain vector that was 32 KiB of memset per read, all of it
  over bytes the transport was about to overwrite.
- `CROSSBOOK_NATIVE` and the `release-native` preset: opt-in `-march=native`
  (`/arch:AVX2` on MSVC) plus LTO, for measuring the ceiling on one's own
  hardware. Off by default, and the README's numbers stay on plain release,
  because a binary tuned to the build machine dies on the next machine.
- The no-allocation probe now covers the decoder, `Feed::handle` end to end
  with the checksum verified, and the frame reader's poll loop. It covered
  the book — 0.3% of the frame — while the claim it enforces is about the
  whole hot path.
- `LATENCY-ROADMAP.md`: where the gap to professional software trading
  systems actually is (environment tail, JSON ceiling, compute already
  competitive) and the phase order for closing it, with sources.
- `json::for_each_member`: walk an object's members once, in wire order,
  dispatching on key. A completed walk carries `well_formed`'s full guarantee,
  which is what lets the decoders below drop their separate validation pass.
- `is_canonical_at_scale(text, scale, mantissa)`: the ingest guard for a token
  the caller has already parsed, skipping the redundant re-parse. Both
  overloads share the same definition of canonical and cannot disagree.
- `kraken_checksum_payload_into`: the checksum payload written into a
  caller-owned buffer. The verifier and the divergence log's payload now go
  through the same bytes by construction.
- `replay_sweep` and `crossbook_verify --sweep`: the rate-vs-latency curve
  over a recorded capture, tiled to at least 10,000 samples per rung, with a
  stated p99 bound defining the knee, thin-tail rungs footnoted rather than
  reported, and early stop once the consumer has fallen behind hard. The
  `ReplayOptions::speed` knob existed from the start; an audit pointed out
  that nothing ever swept it.
- `SKIP_RETURN_CODE 4` on test discovery: Catch2 exits 4 on an all-SKIP run,
  and ctest was reporting the `[timing]` contention probes' honest skips as
  failures.

### Changed

- The Schannel decrypt path copies plaintext once, straight into the caller's
  buffer, instead of twice through an intermediate; and the unconsumed tail of
  a pipelined TLS record is moved in place rather than through a freshly
  allocated vector, which was a heap allocation on the common path — a busy
  feed routinely lands the next record behind the current one in the same
  segment.
- Both venue decoders are single-pass. A Kraken frame was being walked ~9x —
  a `well_formed` pre-pass plus a `find` restart per field, with `checksum`
  and `timestamp` spelled after the level arrays on the wire so each of those
  lookups re-walked both arrays. Decode semantics are pinned unchanged by the
  existing venue tests, the fuzz corpus, and the committed captures; the
  duplicate-key first-wins policy and the kIgnored/kMalformed boundaries are
  additionally pinned by new tests in `test_json.cpp`.
- CRC32 is slice-by-8 over one buffered payload instead of byte-at-a-time fed
  ~12 bytes per level. SSE4.2's crc32 instruction remains unusable here — it
  implements CRC32C, and Kraken checksums with IEEE 0xEDB88320 — and the
  header now records that so nobody re-attempts it.

### Changed — breaking for anyone who relied on inherited warning flags

- `crossbook::crossbook` no longer links `crossbook_warnings`, so consuming the
  library no longer imposes `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wdouble-promotion -Wformat=2`
  on GCC/Clang, or `/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8` on
  MSVC, on the consumer's own translation units. Those are opinions about this
  repository's code, and exporting them meant a desk with an existing codebase
  and `-Werror` got thousands of diagnostics in files that have nothing to do
  with order books — while `/permissive-` quietly changed name lookup and
  two-phase template rules underneath them. If you were relying on the
  inherited flags, link `crossbook_warnings` explicitly or set your own.
- The library's include directory is now marked `SYSTEM` when crossbook is
  consumed as a subproject (`FetchContent` / `add_subdirectory`), so crossbook's
  headers no longer produce warnings in a consumer's build. `find_package`
  consumers already had this: CMake treats an imported target's includes as
  system.

### Added

- **`find_package(crossbook CONFIG REQUIRED)` works.** `install(TARGETS ...
  EXPORT crossbookTargets)`, `install(EXPORT ... NAMESPACE crossbook::)`, a
  generated `crossbookConfig.cmake`, and a `crossbookConfigVersion.cmake` written
  with `COMPATIBILITY SameMinorVersion` and `ARCH_INDEPENDENT`. Before this the
  entire install story was a header copy, so vcpkg, Conan and any prefix-based
  consumption were blocked and the only remaining option was to vendor by
  copy-paste. `ARCH_INDEPENDENT` matters on its own: without it CMake writes a
  `sizeof(void*)` guard into the version file and rejects a header-only package
  on a consumer with a different pointer size, for no reason.
- `CROSSBOOK_INSTALL` option, defaulting to `CROSSBOOK_IS_TOP_LEVEL`. The
  install rules used to be unconditional, so a parent project that vendored
  crossbook and ran `cmake --install .` got crossbook's headers in its prefix
  without asking.
- A self-containment target: one generated translation unit per public header,
  including that header first and twice. Catches the missing-include class of
  bug that the test suite structurally could not, because every test TU includes
  a Catch2 header first and Catch2 drags in most of the standard library. Twice,
  because that also proves `#pragma once` is doing its job.
- On Windows, a translation unit that includes `<windows.h>` **without**
  `NOMINMAX` before every crossbook header, so the `min`/`max` macro hazard
  cannot come back.
- `CROSSBOOK_BUILD_EXAMPLES` and `examples/verify_kraken_frames.cpp`: the
  README's pipeline, compiled and run, exiting non-zero if any of the checksums
  it verifies disagree.
- `CROSSBOOK_CHECK_README_VERSION`, wired into a CI job of its own: fails the
  configure if the `GIT_TAG` in the README's `FetchContent` example is not
  `${PROJECT_VERSION}`.
- CI: a `consumer` job that builds crossbook the way a consumer does — matrixed
  over Linux/Windows and over `FetchContent`/`find_package`, with `-Werror` /
  `/WX` on the consumer's own target. Every previous job configured this repo as
  the top-level project, which is the one configuration in which the packaging
  defects above cannot manifest.
- CI: pinned floor toolchains (CMake 3.24.4, GCC 11, Clang 14) and Debug builds
  on Windows and macOS. The README promises minimums that nothing was testing.
- CI: a compile-only `bench` job, so the target that generates the README's
  performance table cannot rot to a compile error while everything stays green;
  a `format` job on a pinned clang-format; and `actions/cache` for the fuzz
  corpora, which previously started cold on every run.
- `CHANGELOG.md` — this file.

### Fixed

- Missing direct includes in thirteen headers (`<cstddef>`, `<utility>`,
  `<string_view>`, `<cstdint>`, `<algorithm>`, `<iterator>`). They compiled only
  because a sibling crossbook header or a standard library implementation detail
  happened to drag the declaration in; `fixed.hpp`, at the base of the dependency
  tree, was the most exposed.
- `std::min` / `std::max` / `std::clamp` / `numeric_limits<T>::max()` are now
  parenthesised, and `Histogram::min()` / `Histogram::max()` are declared and
  called as `(min)()` / `(max)()`. A consumer translation unit that has seen
  `<windows.h>` without `NOMINMAX` — most of them, on a Windows desk — used to
  get errors *inside crossbook's headers*, which reads as "crossbook is broken".
- `bench/`: dropped the target-wide `-Wno-conversion -Wno-sign-conversion`. It
  was added for Google Benchmark's headers but silenced the flags over
  `bench_book.cpp` too, which is nothing but integer arithmetic on price
  mantissas. Google Benchmark is declared `SYSTEM`, which is the tool for that
  job.

### Changed

- `project(VERSION)` is `0.3.0`, matching the feature set the README describes.
  It said `0.1.0` while the README's own `FetchContent` example pinned
  `v0.1.0` — a tag whose `include/` contains no `feed.hpp` and no
  `venues/kraken.hpp`, both of which the next code block in the README uses.

## [0.3.0] — 2026-08-01

### Added

- L3 order-by-order book: arena-pooled intrusive queues, open-addressed id
  lookup, and queue position (`l3.hpp`).
- `executable_size` and `cost_to_trade` — what can actually be traded at a price
  limit, rather than what the touch implies (`execution.hpp`).
- Consolidated cross-venue book: fee-adjusted, staleness-filtered, with no
  pretence that crypto has an NBBO (`consolidated.hpp`).
- Differential fuzzer for the L3 book against a naive model.

## [0.2.0] — 2026-07-31

### Added

- Kraken v2 `book` and Binance spot/futures depth decoders (`venues/`).
- Feed handler with resnapshot recovery and staleness detection (`feed.hpp`).
- HDR histogram with coordinated-omission correction (`histogram.hpp`).
- Open-loop replay harness that paces frames at their recorded inter-arrival
  times and measures from the instant each was *supposed* to be processed
  (`replay.hpp`).
- Zero-dependency JSON scanner returning raw wire tokens (`json.hpp`) — a parser
  that hands back a `double` has already destroyed the information the checksum
  needs.

### Fixed

- `replay`: a run shorter than one clock tick reports unknown throughput rather
  than zero.

## [0.1.0] — 2026-07-31

### Added

- Exact fixed-point decimal that refuses to round rather than rounding silently
  (`fixed.hpp`).
- L2 book in two implementations — a `std::map` reference and a tick-indexed
  array — differentially tested against each other after every update
  (`book.hpp`).
- Kraken CRC32 checksum, allocation-free, matching the documented algorithm
  (`checksum.hpp`).
- Sequence continuity for Binance spot, Binance futures and Coinbase
  (`sequence.hpp`).
- Divergence log with cause classification (`divergence.hpp`).
- No-allocation hot path enforced by a test that hooks global `operator new`.
- Determinism via state hashing.
- `-Werror`, ASan + UBSan, and a differential fuzzer per subsystem.

### Fixed

- Fuzz oracles were compiled out by `NDEBUG`, so the fuzzers ran without the
  assertions that are the entire point of a differential fuzzer.
