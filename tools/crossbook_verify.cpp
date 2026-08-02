// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// crossbook_verify — rebuild a book from a live venue and check it against the
// venue's own arithmetic, continuously, with a number at the end.
//
// This is the claim in the README made executable. It connects with no API key,
// reconstructs the book from the feed, recomputes Kraken's CRC32 over local
// state on every single update, and reports the match rate together with an
// enumerated list of every disagreement. A match rate without that list is
// marketing; the list is what makes the number checkable.
//
// The same binary replays a capture file offline and must produce the same
// answer, byte for byte, on any platform. That is what CI runs, and it is why
// the number in the README is a regression test rather than an anecdote.
//
// EXIT STATUS IS THE POINT. Any checksum mismatch, sequence gap, or decode
// failure exits non-zero. A verifier that reports problems and then exits
// successfully is a verifier nobody will wire into anything.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
#include "crossbook/json.hpp"
#include "crossbook/net/transport.hpp"
#include "crossbook/net/websocket.hpp"
#include "crossbook/replay.hpp"
#include "crossbook/venues/binance.hpp"
#include "crossbook/venues/kraken.hpp"
#include "tool_common.hpp"

namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

[[nodiscard]] std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::int64_t unix_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Options {
    std::string venue{"kraken"};
    std::string symbol{"BTC/USD"};
    std::string replay_path;
    std::string capture_path;
    int seconds{30};
    int depth{10};
    int price_scale{-1};  ///< -1 means infer from the venue's own spelling.
    int qty_scale{-1};
    double speed{0.0};  ///< >0 enables open-loop paced replay at this multiple.
    int pin_core{-1};   ///< >=0 pins the replay thread to that core.
    bool realtime{false};
    bool quiet{false};
};

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

// ---------------------------------------------------------------------------
// Scale inference
// ---------------------------------------------------------------------------

/// Decimal places in a numeric token, as the venue spelled it.
[[nodiscard]] int decimals_in(std::string_view token) {
    const std::size_t dot = token.find('.');
    if (dot == std::string_view::npos) {
        return 0;
    }
    return static_cast<int>(token.size() - dot - 1);
}

/// Infer an instrument's price and quantity scales from a snapshot frame.
///
/// Not a shortcut around configuration — it is reading the venue's own answer.
/// The checksum identity in fixed.hpp rests on the precondition that values are
/// spelled canonically at the instrument's scale, trailing zeros included, and
/// Kraken's documented example ("0.00100000") shows that it does. Given that,
/// the widest decimal count in a snapshot *is* the scale.
///
/// Getting this wrong produces a book that is numerically correct and fails
/// every checksum, so inferring it beats a hard-coded table that silently rots
/// when a venue changes precision.
[[nodiscard]] bool infer_scales(std::string_view frame, std::string_view price_key,
                                std::string_view qty_key, bool levels_are_arrays,
                                int& price_scale, int& qty_scale, int* depth_out = nullptr) {
    using namespace crossbook;

    int price_max = -1;
    int qty_max = -1;
    int side_levels = 0;
    int max_side_levels = 0;

    auto scan_level = [&](const JsonValue& level) {
        ++side_levels;
        if (levels_are_arrays) {
            // Binance: ["price","qty"].
            int seen = 0;
            (void)json::for_each(level.raw, [&](const JsonValue& field) {
                const std::string_view token = json::number_token(field);
                if (seen == 0) {
                    price_max = (std::max)(price_max, decimals_in(token));
                } else if (seen == 1) {
                    qty_max = (std::max)(qty_max, decimals_in(token));
                }
                ++seen;
                return seen < 2;
            });
        } else {
            // Kraken: {"price":...,"qty":...}.
            price_max = (std::max)(price_max,
                                   decimals_in(json::number_token(json::find(level.raw, price_key))));
            qty_max =
                (std::max)(qty_max, decimals_in(json::number_token(json::find(level.raw, qty_key))));
        }
        return true;
    };

    auto scan_side = [&](std::string_view container, std::string_view key) {
        const JsonValue array = json::find(container, key);
        if (array && array.type == JsonType::kArray) {
            side_levels = 0;
            (void)json::for_each(array.raw, scan_level);
            max_side_levels = (std::max)(max_side_levels, side_levels);
        }
    };

    // Kraken wraps the levels in data[0]; Binance puts them at the top level.
    std::string_view container = frame;
    const JsonValue data = json::find(frame, "data");
    if (data && data.type == JsonType::kArray) {
        (void)json::for_each(data.raw, [&](const JsonValue& entry) {
            container = entry.raw;
            return false;  // First entry only.
        });
    }

    if (levels_are_arrays) {
        scan_side(container, "bids");
        scan_side(container, "asks");
        scan_side(container, "b");
        scan_side(container, "a");
    } else {
        scan_side(container, "bids");
        scan_side(container, "asks");
    }

    if (price_max < 0 || qty_max < 0) {
        return false;
    }
    price_scale = price_max;
    qty_scale = qty_max;
    if (depth_out != nullptr) {
        // A snapshot of a depth-N subscription carries exactly N levels per
        // side, so the capture describes its own depth and a replay needs no
        // extra argument to reproduce the live run.
        *depth_out = max_side_levels;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

template <typename FeedT>
void print_report(const FeedT& feed, const Options& options, double elapsed_s) {
    using namespace crossbook;

    const auto& stats = feed.stats();
    const auto& log = feed.divergences();

    std::printf("\n");
    std::printf("venue                %s  %s\n", options.venue.c_str(), options.symbol.c_str());
    std::printf("scales               price 10^-%d   qty 10^-%d\n", options.price_scale,
                options.qty_scale);
    std::printf("elapsed              %.2f s\n", elapsed_s);
    std::printf("frames               %llu\n", static_cast<unsigned long long>(stats.frames));
    std::printf("  applied            %llu\n", static_cast<unsigned long long>(stats.applied));
    std::printf("  ignored            %llu\n", static_cast<unsigned long long>(stats.ignored));
    std::printf("  rejected           %llu\n", static_cast<unsigned long long>(stats.rejected));
    std::printf("snapshots            %llu\n",
                static_cast<unsigned long long>(stats.snapshots_applied));
    std::printf("resyncs requested    %llu\n",
                static_cast<unsigned long long>(stats.resyncs_requested));
    // Surfaced because zero trims on a depth-limited feed means the depth was
    // never configured, and that is exactly the misconfiguration that produces
    // a book which looks right for minutes and then fails its checksums.
    std::printf("depth / trimmed      %d levels / %llu dropped\n", options.depth,
                static_cast<unsigned long long>(stats.levels_trimmed));

    std::printf("\n");
    std::printf("checksums verified   %llu\n",
                static_cast<unsigned long long>(stats.checksums_verified));
    std::printf("checksum mismatches  %llu\n",
                static_cast<unsigned long long>(stats.checksum_mismatches));

    if (stats.checksums_verified == 0) {
        // Zero verified is not a perfect score. Saying so plainly matters more
        // than the match rate, because 100% of nothing reads identically to
        // 100% of everything on a dashboard.
        std::printf("match rate           n/a - NOTHING WAS VERIFIED\n");
    } else {
        std::printf("match rate           %.6f%%  (%llu of %llu)\n", log.match_rate() * 100.0,
                    static_cast<unsigned long long>(log.verified()),
                    static_cast<unsigned long long>(log.verified() + log.total_recorded()));
    }

    if (!log.entries().empty()) {
        std::printf("\ndivergences (%llu recorded, %llu dropped past the log's capacity)\n",
                    static_cast<unsigned long long>(log.total_recorded()),
                    static_cast<unsigned long long>(log.dropped()));
        // Every one, up to a readable limit. A summarised remainder is exactly
        // the thing this tool refuses to produce.
        std::size_t shown = 0;
        for (const Divergence& d : log.entries()) {
            if (shown++ >= 20) {
                std::printf("  ... and %zu more\n", log.entries().size() - 20);
                break;
            }
            std::printf("  [%s] seq=%llu expected=%llu actual=%llu\n",
                        std::string(to_string(d.kind)).c_str(),
                        static_cast<unsigned long long>(d.sequence),
                        static_cast<unsigned long long>(d.expected),
                        static_cast<unsigned long long>(d.actual));
            if (!d.detail.empty()) {
                std::printf("        %.200s\n", d.detail.c_str());
            }
        }
    }

    Level bid{};
    Level ask{};
    const bool have_bid = feed.book().best(Side::kBid, bid);
    const bool have_ask = feed.book().best(Side::kAsk, ask);
    std::printf("\n");
    std::printf("synced               %s\n", feed.synced() ? "yes" : "NO");
    if (have_bid && have_ask) {
        std::printf("top of book          %s x %s   /   %s x %s\n",
                    format_fixed(bid.price.ticks, static_cast<Scale>(options.price_scale)).c_str(),
                    format_fixed(bid.qty.units, static_cast<Scale>(options.qty_scale)).c_str(),
                    format_fixed(ask.price.ticks, static_cast<Scale>(options.price_scale)).c_str(),
                    format_fixed(ask.qty.units, static_cast<Scale>(options.qty_scale)).c_str());
    }
    std::printf("levels               %zu bid / %zu ask\n", feed.book().bids().size(),
                feed.book().asks().size());
    // The determinism gate: replaying the same capture must reproduce this
    // exact value on every platform and every build.
    std::printf("state hash           %016llx\n",
                static_cast<unsigned long long>(feed.book().state_hash()));
}

/// Non-zero when anything at all disagreed. See the file header.
template <typename FeedT>
[[nodiscard]] int verdict(const FeedT& feed) {
    const auto& stats = feed.stats();
    if (stats.checksums_verified == 0 && stats.applied == 0) {
        std::fprintf(stderr, "\nFAIL: nothing was applied\n");
        return 1;
    }
    if (stats.checksum_mismatches != 0 || stats.rejected != 0 || stats.resyncs_requested != 0) {
        std::fprintf(stderr, "\nFAIL: the book disagreed with the venue\n");
        return 1;
    }
    if (stats.checksums_verified == 0) {
        std::fprintf(stderr, "\nFAIL: no checksum was ever verified\n");
        return 1;
    }
    std::printf("\nPASS: every checked update matched the exchange\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Live
// ---------------------------------------------------------------------------

[[nodiscard]] std::string kraken_subscribe(const std::string& symbol, int depth, bool subscribe) {
    return std::string(R"({"method":")") + (subscribe ? "subscribe" : "unsubscribe") +
           R"(","params":{"channel":"book","symbol":[")" + symbol + R"("],"depth":)" +
           std::to_string(depth) + "}}";
}

int run_kraken_live(Options& options) {
    using namespace crossbook;

    net::WebSocketClient client;
    const std::string url = "wss://ws.kraken.com/v2";
    std::printf("connecting to %s\n", url.c_str());
    if (!tools::connect_with_backoff(client, url)) {
        std::fprintf(stderr, "error: %s\n", client.last_error().c_str());
        return 1;
    }
    if (!client.send_text(kraken_subscribe(options.symbol, options.depth, true))) {
        std::fprintf(stderr, "error: subscribe failed: %s\n", client.last_error().c_str());
        return 1;
    }
    std::printf("subscribed to book %s depth %d\n", options.symbol.c_str(), options.depth);

    CaptureWriter writer;
    if (!options.capture_path.empty() &&
        !writer.open(options.capture_path, "kraken", options.symbol, unix_ns())) {
        std::fprintf(stderr, "error: cannot write %s\n", options.capture_path.c_str());
        return 1;
    }

    // The feed is constructed only once the scales are known, because the
    // decoder needs them and a wrong scale fails every checksum.
    std::unique_ptr<Feed<venues::KrakenBookDecoder>> feed;

    const std::int64_t start = steady_ns();
    const std::int64_t deadline =
        options.seconds > 0 ? start + static_cast<std::int64_t>(options.seconds) * 1'000'000'000LL
                            : 0;
    std::int64_t next_report = start + 1'000'000'000LL;

    for (;;) {
        if (g_stop.load(std::memory_order_relaxed)) {
            std::printf("\ninterrupted\n");
            break;
        }
        if (deadline != 0 && steady_ns() >= deadline) {
            break;
        }

        net::Event event;
        const net::ReadStatus status = client.poll(event);
        if (status == net::ReadStatus::kNeedMore) {
            continue;
        }
        if (status == net::ReadStatus::kClose) {
            std::printf("\npeer closed (code %u)\n", static_cast<unsigned>(event.close_code));
            break;
        }
        if (status != net::ReadStatus::kMessage) {
            std::fprintf(stderr, "\nerror: %s\n", client.last_error().c_str());
            return 1;
        }

        if (writer.is_open() && !writer.write(steady_ns(), event.payload)) {
            std::fprintf(stderr, "error: capture write failed\n");
            return 1;
        }

        if (!feed) {
            // Wait for the book snapshot, which is the only frame carrying
            // enough levels to read the instrument's precision off the wire.
            //
            // Matched structurally rather than by searching for "snapshot" in
            // the text: the subscribe acknowledgement says `"snapshot":true`
            // and carries no levels at all, so a substring test picks the wrong
            // frame and then fails on it.
            if (json::string_body(json::find(event.payload, "channel")) != "book" ||
                json::string_body(json::find(event.payload, "type")) != "snapshot") {
                continue;
            }
            int price_scale = options.price_scale;
            int qty_scale = options.qty_scale;
            if (price_scale < 0 || qty_scale < 0) {
                int inferred_price = 0;
                int inferred_qty = 0;
                if (!infer_scales(event.payload, "price", "qty", false, inferred_price,
                                  inferred_qty)) {
                    continue;  // Not a frame with levels after all; keep waiting.
                }
                if (price_scale < 0) {
                    price_scale = inferred_price;
                }
                if (qty_scale < 0) {
                    qty_scale = inferred_qty;
                }
                std::printf("inferred scales: price 10^-%d, qty 10^-%d\n", price_scale, qty_scale);
            }
            options.price_scale = price_scale;
            options.qty_scale = qty_scale;

            feed = std::make_unique<Feed<venues::KrakenBookDecoder>>(
                "kraken",
                venues::KrakenBookDecoder(InstrumentSpec{options.symbol,
                                                         static_cast<Scale>(price_scale),
                                                         static_cast<Scale>(qty_scale)}),
                SequencePolicy::kStrictIncrement, static_cast<std::size_t>(options.depth));
        }

        const FeedStatus fed = feed->handle(event.payload);
        if (fed == FeedStatus::kNeedsSnapshot) {
            // The book is known to be wrong. Kraken has no resnapshot request,
            // so the recovery is to drop the subscription and take a new one.
            std::printf("\nresyncing after a divergence\n");
            (void)client.send_text(kraken_subscribe(options.symbol, options.depth, false));
            (void)client.send_text(kraken_subscribe(options.symbol, options.depth, true));
        }

        const std::int64_t now = steady_ns();
        if (!options.quiet && now >= next_report) {
            const double elapsed = static_cast<double>(now - start) / 1e9;
            std::printf("\r%6.1fs  %7llu verified  %llu mismatched  %.4f%%   ", elapsed,
                        static_cast<unsigned long long>(feed->stats().checksums_verified),
                        static_cast<unsigned long long>(feed->stats().checksum_mismatches),
                        feed->match_rate() * 100.0);
            (void)std::fflush(stdout);
            next_report = now + 1'000'000'000LL;
        }
    }

    client.close();
    writer.close();

    if (!feed) {
        std::fprintf(stderr, "\nerror: no snapshot arrived; nothing was verified\n");
        return 1;
    }

    print_report(*feed, options, static_cast<double>(steady_ns() - start) / 1e9);
    if (writer.frames() > 0) {
        std::printf("capture              %s (%llu frames)\n", options.capture_path.c_str(),
                    static_cast<unsigned long long>(writer.frames()));
    }
    return verdict(*feed);
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

template <typename FeedT>
int run_replay_with(FeedT& feed, const crossbook::Capture& capture, Options& options) {
    using namespace crossbook;

    const std::int64_t start = steady_ns();

    if (options.speed > 0.0) {
        // Open-loop: paced at the recorded inter-arrival times, so the latency
        // figures include queueing delay rather than measuring service time.
        std::vector<ReplayEvent> events;
        events.reserve(capture.frames().size());
        for (const CapturedFrame& frame : capture.frames()) {
            events.push_back(ReplayEvent{frame.ts_recv, frame.payload});
        }

        // Report the conditions next to the numbers. A latency figure whose
        // measurement conditions are unstated is not a measurement, and a
        // pinning call that silently failed would make the stated conditions a
        // lie rather than merely absent.
        std::printf("\nopen-loop replay at %.1fx\n", options.speed);
        if (options.pin_core >= 0) {
            std::printf("  core               %s %d\n",
                        pin_to_core(options.pin_core) ? "pinned to" : "PIN FAILED for",
                        options.pin_core);
        } else {
            std::puts("  core               not pinned (pass --pin N)");
        }
        if (options.realtime) {
            std::printf("  priority           %s\n",
                        raise_priority() ? "raised" : "RAISE FAILED, normal");
        } else {
            std::puts("  priority           normal (pass --realtime)");
        }

        ReplayOptions replay_options;
        replay_options.speed = options.speed;
        const ReplayResult result = replay_open_loop(
            events, [&](const ReplayEvent& event) { (void)feed.handle(event.frame); },
            replay_options);

        std::printf("  events             %llu\n",
                    static_cast<unsigned long long>(result.events));
        std::printf("  kept pace          %s\n", result.kept_pace() ? "yes" : "NO");
        std::printf("  latency p50/p99    %llu ns / %llu ns\n",
                    static_cast<unsigned long long>(result.latency.p50),
                    static_cast<unsigned long long>(result.latency.p99));
        std::printf("  latency p99.9/max  %llu ns / %llu ns\n",
                    static_cast<unsigned long long>(result.latency.p999),
                    static_cast<unsigned long long>(result.latency.max));
    } else {
        for (const CapturedFrame& frame : capture.frames()) {
            (void)feed.handle(frame.payload);
        }
    }

    print_report(feed, options, static_cast<double>(steady_ns() - start) / 1e9);
    return verdict(feed);
}

int run_replay(Options& options) {
    using namespace crossbook;

    Capture capture;
    std::string error;
    if (!capture.load(options.replay_path, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    if (capture.empty()) {
        std::fprintf(stderr, "error: capture contains no frames\n");
        return 1;
    }

    options.venue = capture.venue();
    options.symbol = capture.symbol();
    std::printf("replaying %s: %s %s, %zu frames, median gap %.2f ms\n",
                options.replay_path.c_str(), capture.venue().c_str(), capture.symbol().c_str(),
                capture.frames().size(), static_cast<double>(capture.median_gap_ns()) / 1e6);

    const bool binance = capture.venue().starts_with("binance");

    // Read the scales off the capture's own snapshot, so a capture is
    // self-describing and a replay needs no arguments beyond the file.
    int price_scale = options.price_scale;
    int qty_scale = options.qty_scale;
    int inferred_depth = 0;
    {
        bool found = false;
        for (const CapturedFrame& frame : capture.frames()) {
            int inferred_price = 0;
            int inferred_qty = 0;
            int depth = 0;
            if (infer_scales(frame.payload, "price", "qty", binance, inferred_price, inferred_qty,
                             &depth)) {
                if (price_scale < 0) {
                    price_scale = inferred_price;
                }
                if (qty_scale < 0) {
                    qty_scale = inferred_qty;
                }
                inferred_depth = depth;
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "error: no frame in the capture carries price levels\n");
            return 1;
        }
        std::printf("inferred scales: price 10^-%d, qty 10^-%d, depth %d\n", price_scale,
                    qty_scale, inferred_depth);
    }
    options.price_scale = price_scale;
    options.qty_scale = qty_scale;

    const InstrumentSpec spec{options.symbol, static_cast<Scale>(price_scale),
                              static_cast<Scale>(qty_scale)};

    if (binance) {
        const venues::BinanceMarket market = capture.venue() == "binance-futures"
                                                 ? venues::BinanceMarket::kFutures
                                                 : venues::BinanceMarket::kSpot;
        venues::BinanceDepthDecoder decoder(spec, market);
        const SequencePolicy policy = decoder.policy();
        // Binance depth streams are diffs over a full book, not a top-N view,
        // so there is nothing to trim.
        Feed<venues::BinanceDepthDecoder> feed(capture.venue(), std::move(decoder), policy);
        return run_replay_with(feed, capture, options);
    }

    Feed<venues::KrakenBookDecoder> feed("kraken", venues::KrakenBookDecoder(spec),
                                         SequencePolicy::kStrictIncrement,
                                         static_cast<std::size_t>(inferred_depth));
    options.depth = inferred_depth;
    return run_replay_with(feed, capture, options);
}

void print_usage() {
    std::printf(
        "crossbook_verify - rebuild a book live and check it against the venue's own checksum\n"
        "\n"
        "Usage:\n"
        "  crossbook_verify [--venue kraken] [--symbol BTC/USD] [--seconds 30] [--capture f]\n"
        "  crossbook_verify --replay <capture> [--speed 1.0]\n"
        "\n"
        "Options:\n"
        "  --venue <name>     kraken (the only venue publishing a checksum)\n"
        "  --symbol <sym>     instrument, in the venue's own spelling (default: BTC/USD)\n"
        "  --depth <n>        book depth to subscribe to (default: 10)\n"
        "  --seconds <n>      run for n seconds; 0 runs until Ctrl-C (default: 30)\n"
        "  --capture <path>   also record the raw feed, for later replay\n"
        "  --replay <path>    verify a recorded capture instead of connecting\n"
        "  --speed <x>        replay open-loop at x times the recorded rate, and\n"
        "                     report latency percentiles measured against the schedule\n"
        "  --pin <n>          pin the replay to one CPU core, so migration is not\n"
        "                     read as latency\n"
        "  --realtime         raise priority for the measurement\n"
        "  --price-scale <n>  override the inferred price precision\n"
        "  --qty-scale <n>    override the inferred quantity precision\n"
        "  --quiet            suppress the progress line\n"
        "\n"
        "Exits non-zero if any checksum mismatched, any frame failed to decode, or\n"
        "nothing was verified at all.\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        auto value = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--venue") {
            options.venue = value("--venue");
        } else if (arg == "--symbol") {
            options.symbol = value("--symbol");
        } else if (arg == "--replay") {
            options.replay_path = value("--replay");
        } else if (arg == "--capture") {
            options.capture_path = value("--capture");
        } else if (arg == "--seconds") {
            options.seconds = std::atoi(value("--seconds"));
        } else if (arg == "--depth") {
            options.depth = std::atoi(value("--depth"));
        } else if (arg == "--price-scale") {
            options.price_scale = std::atoi(value("--price-scale"));
        } else if (arg == "--qty-scale") {
            options.qty_scale = std::atoi(value("--qty-scale"));
        } else if (arg == "--speed") {
            options.speed = std::atof(value("--speed"));
        } else if (arg == "--pin") {
            options.pin_core = std::atoi(value("--pin"));
        } else if (arg == "--realtime") {
            options.realtime = true;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else {
            std::fprintf(stderr, "error: unknown option %.*s\n", static_cast<int>(arg.size()),
                         arg.data());
            print_usage();
            return 2;
        }
    }

    (void)std::signal(SIGINT, on_signal);
#ifdef SIGTERM
    (void)std::signal(SIGTERM, on_signal);
#endif

    if (!options.replay_path.empty()) {
        return run_replay(options);
    }
    if (options.venue != "kraken") {
        // Binance publishes no checksum, so there is nothing to verify against
        // live beyond sequence continuity. Saying so is better than printing a
        // match rate over zero checks.
        std::fprintf(stderr,
                     "error: only kraken publishes a checksum to verify against live.\n"
                     "       use crossbook_capture for other venues, then replay the capture.\n");
        return 2;
    }
    return run_kraken_live(options);
}
