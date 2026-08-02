// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// crossbook_verify — replay a capture of real venue frames through the real
// decoder and book, and report how often the reconstruction matched the
// exchange's own arithmetic.
//
// This is the program that turns the library's central claim from an assertion
// into a measurement. Everything else in the test suite checks crossbook
// against itself or against a specification; this checks it against Kraken.
//
// It prints a match rate AND enumerates every divergence, because a match rate
// with nothing behind it is marketing. "99.7% matched" invites exactly one
// question, and a verifier that cannot answer it has not verified anything.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Platform headers live HERE, in the tool, not in the library. Pinning a core
// and raising priority are the difference between measuring a feed handler and
// measuring the OS scheduler, but they are an application's business: the
// library stays header-only and free of <windows.h>.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "crossbook/capture.hpp"
#include "crossbook/feed.hpp"
#include "crossbook/replay.hpp"
#include "crossbook/venues/kraken.hpp"

using namespace crossbook;

namespace {

struct Options {
    std::string path;
    std::string symbol = "BTC/USD";
    Scale price_scale = 1;
    Scale qty_scale = 8;
    std::size_t max_divergences = 20;
    std::size_t depth = 0;
    bool latency = false;
    double speed = 1.0;
    int pin_core = -1;
    bool realtime = false;
};

void usage() {
    std::puts(
        "usage: crossbook_verify <capture.cbcap> [options]\n"
        "\n"
        "  --symbol S        instrument symbol            (default BTC/USD)\n"
        "  --price-scale N   decimals in price            (default 1)\n"
        "  --qty-scale N     decimals in quantity         (default 8)\n"
        "  --depth N         subscribed book depth, 0=full (default 0)\n"
        "  --show N          divergences to print         (default 20)\n"
        "  --latency         also run an open-loop replay and report percentiles\n"
        "  --speed X         replay speed multiplier for --latency (default 1.0)\n"
        "  --pin N           pin to one CPU core, so migration is not read as latency\n"
        "  --realtime        raise priority for the measurement\n"
        "\n"
        "Produce a capture with: python tools/capture_kraken.py");
}

[[nodiscard]] bool parse_args(int argc, char** argv, Options& out) {
    if (argc < 2) {
        return false;
    }
    out.path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (arg == "--symbol") {
            const char* v = next();
            if (v == nullptr) return false;
            out.symbol = v;
        } else if (arg == "--price-scale") {
            const char* v = next();
            if (v == nullptr) return false;
            out.price_scale = static_cast<Scale>(std::atoi(v));
        } else if (arg == "--qty-scale") {
            const char* v = next();
            if (v == nullptr) return false;
            out.qty_scale = static_cast<Scale>(std::atoi(v));
        } else if (arg == "--show") {
            const char* v = next();
            if (v == nullptr) return false;
            out.max_divergences = static_cast<std::size_t>(std::atoi(v));
        } else if (arg == "--depth") {
            const char* v = next();
            if (v == nullptr) return false;
            out.depth = static_cast<std::size_t>(std::atoi(v));
        } else if (arg == "--pin") {
            const char* v = next();
            if (v == nullptr) return false;
            out.pin_core = std::atoi(v);
        } else if (arg == "--realtime") {
            out.realtime = true;
        } else if (arg == "--latency") {
            out.latency = true;
        } else if (arg == "--speed") {
            const char* v = next();
            if (v == nullptr) return false;
            out.speed = std::atof(v);
        } else {
            std::printf("unknown option: %.*s\n", static_cast<int>(arg.size()), arg.data());
            return false;
        }
    }
    return true;
}

/// Pin this thread to one core.
///
/// Without it the scheduler may migrate the replay mid-run, and the cold caches
/// on the new core land in the histogram as latency the book never caused.
/// Returns false when unsupported, so the caller reports the real conditions
/// rather than silently claiming ones it did not get.
[[nodiscard]] bool pin_to_core(int core) {
#if defined(_WIN32)
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core;
    return false;  // macOS exposes only advisory affinity hints.
#endif
}

/// Raise scheduling priority for the measurement.
///
/// This does not make the code faster. It reduces the chance that an unrelated
/// process preempts the replay and drops a multi-millisecond outlier into the
/// tail — an outlier that is real, but describes the machine rather than the
/// library.
[[nodiscard]] bool raise_priority() {
#if defined(_WIN32)
    // HIGH rather than REALTIME on purpose: real-time priority on Windows can
    // starve input handling and the kernel, which is a hostile thing for a
    // benchmark to do to the machine running it.
    const bool process_ok = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    const bool thread_ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
    return process_ok && thread_ok;
#elif defined(__linux__)
    sched_param param{};
    param.sched_priority = 10;
    // Needs CAP_SYS_NICE; failing is normal unprivileged and is reported, not fatal.
    return sched_setscheduler(0, SCHED_FIFO, &param) == 0;
#else
    return false;
#endif
}

/// Escape and clip a payload so a divergence line stays readable.
std::string preview(std::string_view text, std::size_t limit = 96) {
    std::string out;
    out.reserve(std::min(text.size(), limit) + 3);
    for (std::size_t i = 0; i < text.size() && out.size() < limit; ++i) {
        const char c = text[i];
        out.push_back((c >= 32 && c < 127) ? c : '.');
    }
    if (text.size() > limit) {
        out += "...";
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        usage();
        return 2;
    }

    Capture capture;
    const CaptureError error = capture.load(options.path);
    if (error != CaptureError::kOk) {
        std::printf("failed to read %s: %.*s\n", options.path.c_str(),
                    static_cast<int>(to_string(error).size()), to_string(error).data());
        return 1;
    }
    if (capture.empty()) {
        std::puts("capture contains no frames");
        return 1;
    }

    const double seconds = static_cast<double>(capture.duration_ns()) / 1e9;
    std::printf("capture   %s\n", options.path.c_str());
    std::printf("  frames  %zu over %.1fs (%.0f KiB)\n", capture.size(), seconds,
                static_cast<double>(capture.payload_bytes()) / 1024.0);
    std::printf("  symbol  %s  price_scale=%d qty_scale=%d\n\n", options.symbol.c_str(),
                static_cast<int>(options.price_scale), static_cast<int>(options.qty_scale));

    // Kraken's book channel carries no sequence numbers: the CRC32 on every
    // message IS the continuity check, so a mismatch is the only signal there
    // is and it means resubscribe.
    using KrakenFeed = Feed<venues::KrakenBookDecoder, ArrayBook>;
    KrakenFeed feed("kraken",
                    venues::KrakenBookDecoder(
                        InstrumentSpec{options.symbol, options.price_scale, options.qty_scale}),
                    SequencePolicy::kStrictIncrement, options.depth);

    std::uint64_t applied = 0;
    std::uint64_t ignored = 0;
    std::uint64_t rejected = 0;
    std::uint64_t resyncs = 0;

    for (const ReplayEvent& event : capture.events()) {
        switch (feed.handle(event.frame)) {
            case FeedStatus::kApplied:
                ++applied;
                break;
            case FeedStatus::kIgnored:
                ++ignored;
                break;
            case FeedStatus::kRejected:
                ++rejected;
                break;
            case FeedStatus::kNeedsSnapshot:
                // In a live client this triggers a resubscribe. Replaying a
                // capture there is nothing to re-request, so the run continues
                // and the divergence is recorded — which is exactly what we
                // want to count.
                ++resyncs;
                break;
        }
    }

    const DivergenceLog& log = feed.divergences();

    std::puts("verification");
    std::printf("  applied            %llu\n", static_cast<unsigned long long>(applied));
    std::printf("  ignored            %llu\n", static_cast<unsigned long long>(ignored));
    std::printf("  rejected           %llu\n", static_cast<unsigned long long>(rejected));
    std::printf("  resync requested   %llu\n", static_cast<unsigned long long>(resyncs));
    std::printf("  checksums verified %llu\n",
                static_cast<unsigned long long>(feed.stats().checksums_verified));
    std::printf("  checksum mismatch  %llu\n",
                static_cast<unsigned long long>(feed.stats().checksum_mismatches));

    // A match rate is only evidence if something was actually checked. An
    // untouched log reports 1.0, and reading that as success is the single
    // easiest way to fool yourself with this tool.
    if (log.verified() == 0) {
        std::puts("\n  NO CHECKSUMS WERE VERIFIED. The match rate below means nothing.");
        std::puts("  Check the symbol and scales match the capture.");
    }
    std::printf("  match rate         %.6f%%\n", log.match_rate() * 100.0);

    if (log.total_recorded() > 0) {
        std::printf("\ndivergences (%llu recorded",
                    static_cast<unsigned long long>(log.total_recorded()));
        if (log.dropped() > 0) {
            std::printf(", %llu dropped past the log capacity",
                        static_cast<unsigned long long>(log.dropped()));
        }
        std::puts(")");

        for (const DivergenceKind kind :
             {DivergenceKind::kChecksumMismatch, DivergenceKind::kSequenceGap,
              DivergenceKind::kPrecisionLoss, DivergenceKind::kMalformedMessage,
              DivergenceKind::kStaleFeed}) {
            const std::uint64_t count = log.count(kind);
            if (count > 0) {
                std::printf("  %-20.*s %llu\n", static_cast<int>(to_string(kind).size()),
                            to_string(kind).data(), static_cast<unsigned long long>(count));
            }
        }

        std::puts("\nfirst divergences:");
        std::size_t shown = 0;
        for (const Divergence& d : log.entries()) {
            if (shown++ >= options.max_divergences) {
                break;
            }
            std::printf("  [%.*s] seq=%llu expected=%llu actual=%llu\n",
                        static_cast<int>(to_string(d.kind).size()), to_string(d.kind).data(),
                        static_cast<unsigned long long>(d.sequence),
                        static_cast<unsigned long long>(d.expected),
                        static_cast<unsigned long long>(d.actual));
            if (!d.detail.empty()) {
                const std::string shown_detail = preview(d.detail);
                std::printf("      %s\n", shown_detail.c_str());
            }
        }
    } else {
        std::puts("\ndivergences: none");
    }

    if (options.latency) {
        // A second pass, paced open-loop. Separate from verification because
        // verification should run as fast as the disk allows, while latency
        // needs the original arrival timing.
        std::puts("\nlatency (open-loop, measured against the recorded schedule)");

        // Report the conditions next to the numbers. A latency figure whose
        // measurement conditions are unstated is not a measurement, and a
        // pinning call that silently failed would make the stated conditions a
        // lie rather than merely absent.
        if (options.pin_core >= 0) {
            std::printf("  core     %s %d\n",
                        pin_to_core(options.pin_core) ? "pinned to" : "PIN FAILED for",
                        options.pin_core);
        } else {
            std::puts("  core     not pinned (pass --pin N)");
        }
        if (options.realtime) {
            std::printf("  priority %s\n", raise_priority() ? "raised" : "RAISE FAILED, normal");
        } else {
            std::puts("  priority normal (pass --realtime)");
        }

        KrakenFeed timed("kraken",
                         venues::KrakenBookDecoder(InstrumentSpec{
                             options.symbol, options.price_scale, options.qty_scale}),
                         SequencePolicy::kStrictIncrement, options.depth);

        ReplayOptions replay_options;
        replay_options.speed = options.speed;
        replay_options.warmup_events = std::min<std::size_t>(capture.size() / 20, 500);

        const ReplayResult result = replay_open_loop(
            capture.events(), [&](const ReplayEvent& e) { (void)timed.handle(e.frame); },
            replay_options);

        std::printf("  events   %llu (%llu warm-up discarded)\n",
                    static_cast<unsigned long long>(result.events),
                    static_cast<unsigned long long>(result.skipped));
        std::printf("  p50      %llu ns\n", static_cast<unsigned long long>(result.latency.p50));
        std::printf("  p99      %llu ns\n", static_cast<unsigned long long>(result.latency.p99));
        std::printf("  p99.9    %llu ns\n", static_cast<unsigned long long>(result.latency.p999));
        std::printf("  p99.99   %llu ns\n",
                    static_cast<unsigned long long>(result.latency.p9999));
        std::printf("  max      %llu ns\n", static_cast<unsigned long long>(result.latency.max));

        if (!result.kept_pace()) {
            std::printf(
                "\n  WARNING: fell behind on %llu events (max lateness %llu ns).\n"
                "  These percentiles describe a saturated system, not steady state.\n",
                static_cast<unsigned long long>(result.behind_schedule),
                static_cast<unsigned long long>(result.max_lateness));
        }
    }

    // Non-zero exit when the book disagreed with the exchange, so this is
    // usable as a CI gate rather than only as a report.
    return feed.stats().checksum_mismatches == 0 ? 0 : 1;
}
