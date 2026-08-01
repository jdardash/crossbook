// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// crossbook_capture — connect to a venue, record what it sends.
//
// This is the tool that makes the rest of the repository checkable by someone
// who is not me. It dials a public crypto websocket with no API key, records
// every frame verbatim alongside the instant it arrived, and writes a capture
// file that replays deterministically.
//
// It deliberately does not decode, does not build a book, and does not verify
// anything. Recording and interpreting are separate jobs, and keeping them
// separate means a capture is evidence rather than output: if the book
// implementation changes, the capture is still the same bytes the exchange
// sent, and the new implementation can be held to it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "crossbook/capture.hpp"
#include "crossbook/net/websocket.hpp"
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

struct Venue {
    std::string url;
    std::string subscribe;  ///< Empty when the venue selects the stream by path.
    std::string name;
};

/// Lower-case a symbol and drop separators: "BTC/USD" becomes "btcusd", which is
/// the shape Binance stream names take.
[[nodiscard]] std::string binance_stream_symbol(std::string_view symbol) {
    std::string out;
    out.reserve(symbol.size());
    for (const char c : symbol) {
        if (c == '/' || c == '-' || c == '_') {
            continue;
        }
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

[[nodiscard]] bool build_venue(std::string_view name, std::string_view symbol, int depth,
                               Venue& out) {
    if (name == "kraken") {
        out.name = "kraken";
        out.url = "wss://ws.kraken.com/v2";
        // depth must be one of Kraken's supported book depths; the checksum
        // covers the top 10 regardless, so 10 is the useful default.
        out.subscribe = std::string(R"({"method":"subscribe","params":{"channel":"book",)") +
                        R"("symbol":[")" + std::string(symbol) + R"("],"depth":)" +
                        std::to_string(depth) + "}}";
        return true;
    }
    if (name == "binance") {
        out.name = "binance";
        // 100ms diff-depth: the fastest cadence the public spot stream offers.
        out.url = "wss://stream.binance.com:9443/ws/" + binance_stream_symbol(symbol) +
                  "@depth@100ms";
        return true;
    }
    if (name == "binance-futures") {
        out.name = "binance-futures";
        out.url = "wss://fstream.binance.com/ws/" + binance_stream_symbol(symbol) + "@depth@100ms";
        return true;
    }
    return false;
}

void print_usage() {
    std::printf(
        "crossbook_capture - record a venue's raw websocket feed\n"
        "\n"
        "Usage:\n"
        "  crossbook_capture [options]\n"
        "\n"
        "Options:\n"
        "  --venue <name>     kraken | binance | binance-futures  (default: kraken)\n"
        "  --symbol <sym>     instrument, in the venue's own spelling (default: BTC/USD)\n"
        "  --depth <n>        book depth to request, Kraken only (default: 10)\n"
        "  --seconds <n>      stop after n seconds; 0 runs until Ctrl-C (default: 30)\n"
        "  --out <path>       write a capture file (default: none, count only)\n"
        "  --url <url>        dial this URL instead of a venue preset\n"
        "  --subscribe <json> send this after connecting; use with --url\n"
        "  --quiet            suppress the periodic progress line\n"
        "  --help             this text\n"
        "\n"
        "Examples:\n"
        "  crossbook_capture --venue kraken --symbol BTC/USD --seconds 60 --out btc.cbcap\n"
        "  crossbook_capture --venue binance --symbol BTCUSDT --seconds 10\n"
        "\n"
        "Both venues are public: no API key, no account, nothing to configure.\n");
}

/// Percentile over a sorted vector, using the nearest-rank convention so the
/// answer is always a value that was actually observed.
[[nodiscard]] std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    auto index = static_cast<std::size_t>((p / 100.0) * static_cast<double>(sorted.size()));
    if (index >= sorted.size()) {
        index = sorted.size() - 1;
    }
    return sorted[index];
}

}  // namespace

int main(int argc, char** argv) {
    std::string venue_name = "kraken";
    std::string symbol = "BTC/USD";
    std::string out_path;
    std::string custom_url;
    std::string custom_subscribe;
    int depth = 10;
    int seconds = 30;
    bool quiet = false;

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
            venue_name = value("--venue");
        } else if (arg == "--symbol") {
            symbol = value("--symbol");
        } else if (arg == "--out") {
            out_path = value("--out");
        } else if (arg == "--url") {
            custom_url = value("--url");
        } else if (arg == "--subscribe") {
            custom_subscribe = value("--subscribe");
        } else if (arg == "--depth") {
            depth = std::atoi(value("--depth"));
        } else if (arg == "--seconds") {
            seconds = std::atoi(value("--seconds"));
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::fprintf(stderr, "error: unknown option %.*s\n", static_cast<int>(arg.size()),
                         arg.data());
            print_usage();
            return 2;
        }
    }

    Venue venue;
    if (!custom_url.empty()) {
        venue.name = "custom";
        venue.url = custom_url;
        venue.subscribe = custom_subscribe;
    } else if (!build_venue(venue_name, symbol, depth, venue)) {
        std::fprintf(stderr, "error: unknown venue '%s'\n", venue_name.c_str());
        return 2;
    }

    (void)std::signal(SIGINT, on_signal);
#ifdef SIGTERM
    (void)std::signal(SIGTERM, on_signal);
#endif

    crossbook::net::WebSocketClient client;
    std::printf("connecting to %s\n", venue.url.c_str());

    if (!crossbook::tools::connect_with_backoff(client, venue.url)) {
        std::fprintf(stderr, "error: %s\n", client.last_error().c_str());
        return 1;
    }
    std::printf("connected: %s%s\n", client.url().host.c_str(), client.url().path.c_str());

    if (!venue.subscribe.empty()) {
        if (!client.send_text(venue.subscribe)) {
            std::fprintf(stderr, "error: subscribe failed: %s\n", client.last_error().c_str());
            return 1;
        }
        std::printf("subscribed: %s\n", venue.subscribe.c_str());
    }

    crossbook::CaptureWriter writer;
    if (!out_path.empty()) {
        if (!writer.open(out_path, venue.name, symbol, unix_ns())) {
            std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
            return 1;
        }
        std::printf("recording to %s\n", out_path.c_str());
    }

    const std::int64_t start = steady_ns();
    const std::int64_t deadline =
        seconds > 0 ? start + static_cast<std::int64_t>(seconds) * 1'000'000'000LL : 0;

    std::vector<std::int64_t> gaps;
    gaps.reserve(1 << 16);

    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::int64_t previous = 0;
    std::int64_t next_report = start + 1'000'000'000LL;
    int exit_code = 0;

    for (;;) {
        if (g_stop.load(std::memory_order_relaxed)) {
            std::printf("\ninterrupted\n");
            break;
        }
        if (deadline != 0 && steady_ns() >= deadline) {
            break;
        }

        crossbook::net::Event event;
        const crossbook::net::ReadStatus status = client.poll(event);

        if (status == crossbook::net::ReadStatus::kMessage) {
            const std::int64_t now = steady_ns();
            ++frames;
            bytes += event.payload.size();
            if (previous != 0) {
                gaps.push_back(now - previous);
            }
            previous = now;

            if (writer.is_open() && !writer.write(now, event.payload)) {
                std::fprintf(stderr, "error: capture write failed\n");
                exit_code = 1;
                break;
            }
        } else if (status == crossbook::net::ReadStatus::kNeedMore) {
            // Read timeout: a quiet market, not a problem. Loop round so the
            // deadline and the signal flag still get checked.
            continue;
        } else if (status == crossbook::net::ReadStatus::kClose) {
            std::printf("\npeer closed (code %u)\n", static_cast<unsigned>(event.close_code));
            break;
        } else {
            std::fprintf(stderr, "\nerror: %s\n", client.last_error().c_str());
            exit_code = 1;
            break;
        }

        const std::int64_t now = steady_ns();
        if (!quiet && now >= next_report) {
            const double elapsed = static_cast<double>(now - start) / 1e9;
            std::printf("\r%6.1fs  %8llu frames  %7.1f/s  %8.2f MiB", elapsed,
                        static_cast<unsigned long long>(frames),
                        static_cast<double>(frames) / (elapsed > 0 ? elapsed : 1),
                        static_cast<double>(bytes) / (1024.0 * 1024.0));
            (void)std::fflush(stdout);
            next_report = now + 1'000'000'000LL;
        }
    }

    client.close();
    writer.close();

    const double elapsed = static_cast<double>(steady_ns() - start) / 1e9;
    std::sort(gaps.begin(), gaps.end());

    std::printf("\n\n");
    std::printf("venue                %s  %s\n", venue.name.c_str(), symbol.c_str());
    std::printf("elapsed              %.2f s\n", elapsed);
    std::printf("messages             %llu  (%.1f/s)\n", static_cast<unsigned long long>(frames),
                static_cast<double>(frames) / (elapsed > 0 ? elapsed : 1));
    std::printf("bytes                %.2f MiB\n", static_cast<double>(bytes) / (1024.0 * 1024.0));
    std::printf("pings answered       %llu\n",
                static_cast<unsigned long long>(client.stats().pongs_sent));

    if (!gaps.empty()) {
        // Inter-arrival, not latency: this is how often the venue speaks, and it
        // says nothing about how fast anything here is. The distinction matters
        // enough to spell out in the output rather than leave to the reader.
        std::printf("inter-arrival gaps   p50 %.2f ms   p99 %.2f ms   max %.2f ms\n",
                    static_cast<double>(percentile(gaps, 50)) / 1e6,
                    static_cast<double>(percentile(gaps, 99)) / 1e6,
                    static_cast<double>(gaps.back()) / 1e6);
    }
    if (writer.frames() > 0) {
        std::printf("capture              %s  (%llu frames, %.2f MiB)\n", out_path.c_str(),
                    static_cast<unsigned long long>(writer.frames()),
                    static_cast<double>(writer.bytes()) / (1024.0 * 1024.0));
    }

    if (frames == 0 && exit_code == 0) {
        // Connecting and then receiving nothing is a failure, not a quiet
        // success. Exiting zero here would let a broken subscription pass CI.
        std::fprintf(stderr, "error: connected but received no messages\n");
        exit_code = 1;
    }
    return exit_code;
}
