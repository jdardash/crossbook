// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Capture files: recorded wire frames with local receive timestamps.
//
// WHY A FILE FORMAT AT ALL
//
// A correctness claim that can only be checked against a live socket is a
// correctness claim nobody else can check. Recording the exact bytes a venue
// sent, with the time they arrived, makes three things possible:
//
//   - CI can verify against real venue data without a network connection.
//   - A divergence is reproducible: the input that caused it is on disk.
//   - The replay harness has something to pace, so latency figures describe
//     real message arrival patterns rather than a synthetic generator's.
//
// The format is deliberately trivial — magic, then length-prefixed records —
// because a capture format that needs a parser is a second thing that can be
// wrong about the data. Payloads are stored verbatim, byte for byte, with no
// normalisation: the point is to preserve exactly what the wire carried,
// including the whitespace and number spelling the checksum depends on.
//
//     magic   8 bytes  "CBCAP1\0\0"
//     record  int64 ts_recv_ns (LE), uint32 length (LE), length bytes
//
// Written by tools/capture_kraken.py, read here.

#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "crossbook/replay.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

inline constexpr char kCaptureMagic[8] = {'C', 'B', 'C', 'A', 'P', '1', '\0', '\0'};

/// Why a capture could not be read.
enum class CaptureError : std::uint8_t {
    kOk,
    kNotFound,
    kBadMagic,
    kTruncated,
    /// A record claimed a length that cannot be real. Refused rather than
    /// allocated: a corrupt capture must not be able to exhaust memory.
    kImplausibleRecord,
};

[[nodiscard]] constexpr std::string_view to_string(CaptureError e) noexcept {
    switch (e) {
        case CaptureError::kOk:
            return "ok";
        case CaptureError::kNotFound:
            return "not_found";
        case CaptureError::kBadMagic:
            return "bad_magic";
        case CaptureError::kTruncated:
            return "truncated";
        case CaptureError::kImplausibleRecord:
            return "implausible_record";
    }
    return "unknown";
}

/// Largest single frame accepted. Venue frames are kilobytes; anything past
/// this is corruption or hostility.
inline constexpr std::uint32_t kMaxCaptureRecord = 64u * 1024u * 1024u;

/// A capture held in memory: one contiguous payload buffer plus views into it.
///
/// Frames are views rather than copies so replaying a large capture does not
/// duplicate it, and so the bytes handed to the decoder are literally the bytes
/// that came off the wire.
class Capture {
public:
    [[nodiscard]] const std::vector<ReplayEvent>& events() const noexcept { return events_; }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }

    /// Wall-clock span of the capture in nanoseconds.
    [[nodiscard]] Timestamp duration_ns() const noexcept {
        return events_.size() < 2 ? 0 : events_.back().ts_recv - events_.front().ts_recv;
    }

    [[nodiscard]] std::size_t payload_bytes() const noexcept { return payload_.size(); }

    /// Read a capture file. Returns kOk on success.
    ///
    /// A truncated tail is reported rather than silently accepted: a capture cut
    /// off mid-record would otherwise hand a partial frame to the decoder, and
    /// "the last frame was garbage" is not something a verification run should
    /// discover as a divergence.
    [[nodiscard]] CaptureError load(const std::string& path) {
        events_.clear();
        payload_.clear();

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return CaptureError::kNotFound;
        }

        char magic[sizeof(kCaptureMagic)] = {};
        file.read(magic, sizeof(magic));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(magic)) ||
            std::memcmp(magic, kCaptureMagic, sizeof(magic)) != 0) {
            return CaptureError::kBadMagic;
        }

        // Two passes would mean reading the file twice; instead collect
        // (offset, length) and resolve to views after the buffer stops growing,
        // since reallocation invalidates any view taken during the read.
        struct Record {
            Timestamp ts;
            std::size_t offset;
            std::size_t length;
        };
        std::vector<Record> records;

        for (;;) {
            std::int64_t ts = 0;
            std::uint32_t length = 0;
            file.read(reinterpret_cast<char*>(&ts), sizeof(ts));
            if (file.gcount() == 0) {
                break;  // Clean end of file.
            }
            if (file.gcount() != static_cast<std::streamsize>(sizeof(ts))) {
                return CaptureError::kTruncated;
            }
            file.read(reinterpret_cast<char*>(&length), sizeof(length));
            if (file.gcount() != static_cast<std::streamsize>(sizeof(length))) {
                return CaptureError::kTruncated;
            }
            if (length > kMaxCaptureRecord) {
                return CaptureError::kImplausibleRecord;
            }

            const std::size_t offset = payload_.size();
            payload_.resize(offset + length);
            if (length > 0) {
                file.read(payload_.data() + offset, static_cast<std::streamsize>(length));
                if (file.gcount() != static_cast<std::streamsize>(length)) {
                    return CaptureError::kTruncated;
                }
            }
            records.push_back(Record{ts, offset, length});
        }

        events_.reserve(records.size());
        for (const Record& record : records) {
            events_.push_back(
                ReplayEvent{record.ts, std::string_view(payload_.data() + record.offset,
                                                        record.length)});
        }
        return CaptureError::kOk;
    }

private:
    std::vector<char> payload_;
    std::vector<ReplayEvent> events_;
};

/// Append-only capture writer, so a C++ process can record what it received.
class CaptureWriter {
public:
    [[nodiscard]] bool open(const std::string& path) {
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_) {
            return false;
        }
        file_.write(kCaptureMagic, sizeof(kCaptureMagic));
        return static_cast<bool>(file_);
    }

    [[nodiscard]] bool write(Timestamp ts_recv, std::string_view frame) {
        if (!file_ || frame.size() > kMaxCaptureRecord) {
            return false;
        }
        const std::int64_t ts = ts_recv;
        const auto length = static_cast<std::uint32_t>(frame.size());
        file_.write(reinterpret_cast<const char*>(&ts), sizeof(ts));
        file_.write(reinterpret_cast<const char*>(&length), sizeof(length));
        file_.write(frame.data(), static_cast<std::streamsize>(frame.size()));
        ++records_;
        return static_cast<bool>(file_);
    }

    void close() { file_.close(); }
    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }

private:
    std::ofstream file_;
    std::uint64_t records_{0};
};

}  // namespace crossbook
