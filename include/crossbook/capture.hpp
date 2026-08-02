// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Capture files: raw venue frames plus the instant each one arrived.
//
// WHY THIS FILE IS THE POINT OF THE TRANSPORT LAYER
//
// The claim this repository makes — that the book is verified against the
// exchange's own arithmetic on live data — is only interesting if someone else
// can check it. In equities they cannot: the equivalent data is licensed and
// cannot be redistributed, which is why every public ITCH order book project
// ships without runnable data and asks to be believed.
//
// Crypto venues have no such restriction, so a capture can simply be committed.
// A capture turns a live measurement into a reproducible one: the same bytes
// replayed on any machine must produce the same book, the same state hash, and
// the same checksum match rate. That makes the number in the README a test
// rather than a claim, and it is what CI replays offline on every push.
//
// THE FORMAT
//
//   line 1     "CBCAP1 <venue> <symbol> <unix_nanos_at_start>\n"
//   then, repeating:
//              "<ts_ns> <byte_count>\n"
//              "<byte_count bytes verbatim>\n"
//
// Length-prefixed rather than one-JSON-object-per-line, for two reasons. The
// bytes are stored exactly as they came off the wire — no escaping, no
// re-encoding, nothing that could make the replayed book differ from the live
// one, which would defeat the entire purpose. And a frame containing a newline
// stays one record instead of silently becoming two.
//
// Timestamps are steady-clock nanoseconds. Only differences are meaningful; the
// epoch is arbitrary and deliberately not wall-clock, because a capture that
// straddles an NTP step should not contain a negative inter-arrival gap.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crossbook {
namespace detail {

/// fopen, without tripping MSVC's deprecation of it.
///
/// A header that forces `_CRT_SECURE_NO_WARNINGS` on everyone who includes it
/// is worse than the four lines it saves — that macro would silence the warning
/// in the consumer's own code too.
[[nodiscard]] inline std::FILE* open_file(const char* path, const char* mode) noexcept {
#ifdef _MSC_VER
    std::FILE* file = nullptr;
    return (::fopen_s(&file, path, mode) == 0) ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

}  // namespace detail

/// One recorded frame. `payload` views into the buffer held by `Capture`.
struct CapturedFrame {
    std::int64_t ts_recv{0};
    std::string_view payload;
};

/// Streaming writer. Frames are appended as they arrive, so a capture that is
/// interrupted is still a valid prefix rather than a lost file.
class CaptureWriter {
public:
    CaptureWriter() = default;

    ~CaptureWriter() { close(); }

    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    [[nodiscard]] bool open(const std::string& path, std::string_view venue,
                            std::string_view symbol, std::int64_t start_unix_ns) {
        close();
        // Binary mode matters on Windows: text mode would translate '\n' into
        // CRLF inside frame payloads and change the bytes being recorded.
        file_ = detail::open_file(path.c_str(), "wb");
        if (file_ == nullptr) {
            return false;
        }
        const int written = std::fprintf(file_, "CBCAP1 %.*s %.*s %lld\n",
                                         static_cast<int>(venue.size()), venue.data(),
                                         static_cast<int>(symbol.size()), symbol.data(),
                                         static_cast<long long>(start_unix_ns));
        return written > 0;
    }

    [[nodiscard]] bool write(std::int64_t ts_recv, std::string_view payload) {
        if (file_ == nullptr) {
            return false;
        }
        if (std::fprintf(file_, "%lld %zu\n", static_cast<long long>(ts_recv),
                         payload.size()) <= 0) {
            return false;
        }
        if (!payload.empty() &&
            std::fwrite(payload.data(), 1, payload.size(), file_) != payload.size()) {
            return false;
        }
        if (std::fputc('\n', file_) == EOF) {
            return false;
        }
        ++frames_;
        bytes_ += payload.size();
        return true;
    }

    void close() {
        if (file_ != nullptr) {
            (void)std::fclose(file_);
            file_ = nullptr;
        }
    }

    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

private:
    std::FILE* file_{nullptr};
    std::uint64_t frames_{0};
    std::uint64_t bytes_{0};
};

/// A loaded capture. Owns the file contents; the frame views point into it.
class Capture {
public:
    [[nodiscard]] const std::string& venue() const noexcept { return venue_; }
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] std::int64_t start_unix_ns() const noexcept { return start_unix_ns_; }
    [[nodiscard]] const std::vector<CapturedFrame>& frames() const noexcept { return frames_; }
    [[nodiscard]] bool empty() const noexcept { return frames_.empty(); }

    /// Parse a capture held in memory.
    ///
    /// Returns false and leaves `error` set on anything it cannot navigate. A
    /// truncated final record is tolerated — a capture cut short by Ctrl-C ends
    /// mid-frame, and refusing to load it would make every interrupted run
    /// worthless.
    [[nodiscard]] bool parse(std::string contents, std::string& error) {
        buffer_ = std::move(contents);
        frames_.clear();

        std::string_view text(buffer_);
        const std::size_t first_eol = text.find('\n');
        if (first_eol == std::string_view::npos) {
            error = "capture has no header line";
            return false;
        }

        std::string_view header = text.substr(0, first_eol);
        if (!header.starts_with("CBCAP1 ")) {
            error = "capture header is not CBCAP1";
            return false;
        }
        header.remove_prefix(7);

        if (!take_field(header, venue_) || !take_field(header, symbol_)) {
            error = "capture header is missing venue or symbol";
            return false;
        }
        std::string start_text;
        (void)take_field(header, start_text);
        start_unix_ns_ = parse_i64(start_text);

        std::size_t pos = first_eol + 1;
        while (pos < text.size()) {
            const std::size_t eol = text.find('\n', pos);
            if (eol == std::string_view::npos) {
                break;  // Truncated record header; see the note above.
            }
            const std::string_view record = text.substr(pos, eol - pos);
            const std::size_t space = record.find(' ');
            if (space == std::string_view::npos) {
                error = "malformed record header: " + std::string(record);
                return false;
            }

            const std::int64_t ts = parse_i64(record.substr(0, space));
            const std::int64_t len = parse_i64(record.substr(space + 1));
            if (len < 0) {
                error = "negative frame length";
                return false;
            }

            const std::size_t body = eol + 1;
            const auto count = static_cast<std::size_t>(len);
            if (body + count > text.size()) {
                break;  // Truncated payload.
            }

            frames_.push_back(CapturedFrame{ts, text.substr(body, count)});
            pos = body + count + 1;  // Skip the record's trailing newline.
        }

        error.clear();
        return true;
    }

    /// Load and parse a capture from disk.
    [[nodiscard]] bool load(const std::string& path, std::string& error) {
        std::FILE* file = detail::open_file(path.c_str(), "rb");
        if (file == nullptr) {
            error = "cannot open capture: " + path;
            return false;
        }
        std::string contents;
        char chunk[65536];
        for (;;) {
            const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file);
            if (got == 0) {
                break;
            }
            contents.append(chunk, got);
        }
        (void)std::fclose(file);
        return parse(std::move(contents), error);
    }

    /// Median inter-arrival gap, nanoseconds. Zero for a capture of fewer than
    /// two frames. Median rather than mean because market data arrives in
    /// bursts and a mean is dominated by the quiet stretches between them.
    [[nodiscard]] std::int64_t median_gap_ns() const {
        if (frames_.size() < 2) {
            return 0;
        }
        std::vector<std::int64_t> gaps;
        gaps.reserve(frames_.size() - 1);
        for (std::size_t i = 1; i < frames_.size(); ++i) {
            const std::int64_t delta = frames_[i].ts_recv - frames_[i - 1].ts_recv;
            gaps.push_back(delta > 0 ? delta : 0);
        }
        const std::size_t mid = gaps.size() / 2;
        std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(mid),
                         gaps.end());
        return gaps[mid];
    }

private:
    /// Pull one space-delimited field off the front of `text`.
    static bool take_field(std::string_view& text, std::string& out) {
        if (text.empty()) {
            return false;
        }
        const std::size_t space = text.find(' ');
        if (space == std::string_view::npos) {
            out.assign(text);
            text = {};
            return !out.empty();
        }
        out.assign(text.substr(0, space));
        text.remove_prefix(space + 1);
        return !out.empty();
    }

    /// Decimal parse that returns -1 rather than throwing. Every caller checks
    /// the result against a range, so a sentinel beats an exception here.
    [[nodiscard]] static std::int64_t parse_i64(std::string_view text) noexcept {
        if (text.empty()) {
            return -1;
        }
        std::int64_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return -1;
            }
            if (value > (9'223'372'036'854'775'807LL - (c - '0')) / 10) {
                return -1;  // Overflow: reject rather than wrap.
            }
            value = value * 10 + (c - '0');
        }
        return value;
    }

    std::string buffer_;
    std::string venue_;
    std::string symbol_;
    std::vector<CapturedFrame> frames_;
    std::int64_t start_unix_ns_{0};
};

}  // namespace crossbook
