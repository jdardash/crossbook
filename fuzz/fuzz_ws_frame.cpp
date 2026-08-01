// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Fuzz the websocket frame reader.
//
// This is the code that reads a 64-bit length off a socket and then indexes with
// it. Every other parser in this repository consumes bytes that have already
// been framed; this one consumes whatever the peer sends, before anything has
// been validated. If exactly one thing in crossbook deserves coverage-guided
// fuzzing, it is this.
//
// The properties asserted go beyond "does not crash", because ASan already
// catches that:
//
//   1. TERMINATION. The reader must never claim progress it did not make. Every
//      call either returns an event, consumes bytes, or asks for more — so a
//      loop over `next` on a fixed buffer must end.
//
//   2. BOUNDS. Every returned payload must lie inside memory the reader owns,
//      and must respect the configured message ceiling. ASan enforces the first
//      violently; the explicit check makes the intent legible.
//
//   3. STICKINESS OF FAILURE. A protocol error is terminal. A reader that
//      reported an error and then resynchronised would be building a book from
//      bytes nobody has a contract for.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fuzz_check.hpp"

#include "crossbook/net/ws_frame.hpp"

using namespace crossbook::net;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }

    // A small ceiling relative to typical inputs, so the too-large path is
    // reached often rather than only on inputs the fuzzer must work to build.
    constexpr std::size_t kMaxMessage = 4096;

    // The first byte picks a chunk size, so the same bytes get delivered with
    // different splits. Frames straddling a read boundary is where the
    // bookkeeping bugs live, and a fuzzer that always appends the whole buffer
    // at once would never explore that.
    const std::size_t chunk = static_cast<std::size_t>(data[0]) + 1;
    const std::uint8_t* payload = data + 1;
    const std::size_t payload_size = size - 1;

    FrameReader reader(kMaxMessage);

    std::size_t offset = 0;
    bool terminal = false;

    while (offset < payload_size && !terminal) {
        const std::size_t take = (chunk < payload_size - offset) ? chunk : (payload_size - offset);
        reader.append(reinterpret_cast<const char*>(payload + offset), take);
        offset += take;

        for (int guard = 0;; ++guard) {
            // The reader consumes at least one frame per event, and a frame is
            // at least two bytes, so the number of events from a bounded buffer
            // is bounded. A run away past that means `next` returned an event
            // without consuming anything.
            CB_CHECK(guard <= static_cast<int>(payload_size) + 2);

            Event event;
            const ReadStatus status = reader.next(event);

            if (status == ReadStatus::kNeedMore) {
                break;
            }
            if (status == ReadStatus::kProtocolError || status == ReadStatus::kMessageTooLarge ||
                status == ReadStatus::kClose) {
                terminal = true;
                break;
            }

            // A message or a control frame: its payload must be within the
            // ceiling. Control frames are separately capped at 125 by the
            // specification, and the reader must be enforcing that too.
            CB_CHECK(event.payload.size() <= kMaxMessage);
            if (is_control(event.opcode)) {
                CB_CHECK(event.payload.size() <= kMaxControlPayload);
            }

            // Touch every byte so ASan checks the whole span, not just the
            // pointer. A view that outlived its buffer would be invisible
            // otherwise.
            volatile std::uint8_t sink = 0;
            for (const char c : event.payload) {
                sink = static_cast<std::uint8_t>(sink ^ static_cast<std::uint8_t>(c));
            }
            (void)sink;
        }
    }

    // Once a failure is latched, the reader must stay failed no matter what is
    // fed to it afterwards. This is the property that stops a corrupted stream
    // from producing a plausible book: there is no resynchronisation point in a
    // stream of length-prefixed frames, so recovery would mean inventing frames.
    if (reader.failed()) {
        reader.append(reinterpret_cast<const char*>(payload), payload_size);
        for (int i = 0; i < 4; ++i) {
            Event event;
            const ReadStatus status = reader.next(event);
            CB_CHECK(status == ReadStatus::kProtocolError ||
                     status == ReadStatus::kMessageTooLarge);
        }
    }

    return 0;
}
