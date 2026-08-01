// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Fuzz the sequence state machine.
//
// The safety property here is not "does not crash" — it is a liveness and
// safety invariant that matters more than any crash:
//
//   ONCE A GAP IS DETECTED, NO FURTHER UPDATE MAY BE APPLIED UNTIL A FRESH
//   SNAPSHOT ARRIVES.
//
// Violating it means silently applying diffs on top of a book that is known to
// be wrong, which produces a plausible-looking book that disagrees with the
// venue in ways no downstream consumer can detect. That is strictly worse than
// crashing, and it is exactly the failure a hand-written test suite tends to
// miss because it requires an adversarial ordering to trigger.

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "crossbook/sequence.hpp"

using namespace crossbook;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }

    const SequencePolicy policy = static_cast<SequencePolicy>(data[0] % 3);
    SequenceTracker tracker(policy);

    std::size_t pos = 1;
    bool gap_seen_since_snapshot = false;

    auto take = [&](std::size_t n) -> std::uint64_t {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n && pos < size; ++i, ++pos) {
            v = (v << 8) | data[pos];
        }
        return v;
    };

    while (pos < size) {
        const std::uint8_t op = static_cast<std::uint8_t>(take(1));

        if ((op & 0x0F) == 0) {
            tracker.on_snapshot(take(2));
            gap_seen_since_snapshot = false;
            // A snapshot always restores sync, whatever came before it.
            assert(tracker.synced());
            assert(!tracker.straddled());
            continue;
        }

        if ((op & 0x0F) == 1) {
            tracker.invalidate();
            gap_seen_since_snapshot = true;
            assert(!tracker.synced());
            continue;
        }

        UpdateIds ids{};
        ids.first_id = take(2);
        ids.final_id = take(2);
        ids.prev_id = take(2);

        const SequenceAction action = tracker.on_update(ids);

        // THE INVARIANT.
        if (gap_seen_since_snapshot) {
            assert(action == SequenceAction::kResyncRequired);
        }
        if (action == SequenceAction::kResyncRequired) {
            gap_seen_since_snapshot = true;
            // Losing sync must be visible to the caller, not just internal.
            assert(!tracker.synced());
        }
        if (action == SequenceAction::kApply) {
            // Applying implies we are synced and have straddled the snapshot.
            assert(tracker.synced());
            assert(tracker.straddled());
            assert(tracker.last_applied_id() == ids.final_id);
        }

        // Stats must never disagree with the actions returned.
        const SequenceStats& s = tracker.stats();
        assert(s.applied + s.discarded_stale + s.gaps_detected <= size);
    }

    return 0;
}
