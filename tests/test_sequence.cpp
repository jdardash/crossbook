// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include "crossbook/sequence.hpp"

using namespace crossbook;

namespace {

UpdateIds spot(SequenceId first, SequenceId final_id) { return UpdateIds{first, final_id, 0}; }
UpdateIds futures(SequenceId first, SequenceId final_id, SequenceId prev) {
    return UpdateIds{first, final_id, prev};
}
UpdateIds strict(SequenceId id) { return UpdateIds{id, id, 0}; }

}  // namespace

// ---------------------------------------------------------------------------
// Universal invariant
// ---------------------------------------------------------------------------

TEST_CASE("no update is applied before a snapshot", "[sequence]") {
    // Applying diffs to an empty book produces a book that looks plausible and
    // is missing everything that was resting before you connected.
    for (auto policy : {SequencePolicy::kBinanceSpot, SequencePolicy::kBinanceFutures,
                        SequencePolicy::kStrictIncrement}) {
        SequenceTracker t(policy);
        CHECK_FALSE(t.synced());
        CHECK(t.on_update(spot(1, 2)) == SequenceAction::kResyncRequired);
    }
}

// ---------------------------------------------------------------------------
// Binance spot: U == previous u + 1
// ---------------------------------------------------------------------------

TEST_CASE("spot discards events entirely below the snapshot", "[sequence][spot]") {
    // Expected while draining the buffer captured during the REST fetch. These
    // are normal, not errors, and must not trip a resync.
    SequenceTracker t(SequencePolicy::kBinanceSpot);
    t.on_snapshot(100);

    CHECK(t.on_update(spot(90, 95)) == SequenceAction::kDiscardStale);
    CHECK(t.on_update(spot(96, 99)) == SequenceAction::kDiscardStale);
    CHECK(t.stats().discarded_stale == 2);
    CHECK(t.stats().gaps_detected == 0);
    CHECK(t.synced());
}

TEST_CASE("spot requires the first applied event to straddle the snapshot", "[sequence][spot]") {
    SECTION("a straddling event is applied") {
        SequenceTracker t(SequencePolicy::kBinanceSpot);
        t.on_snapshot(100);
        CHECK(t.on_update(spot(98, 105)) == SequenceAction::kApply);
        CHECK(t.straddled());
        CHECK(t.last_applied_id() == 105);
    }
    SECTION("U exactly at the snapshot id straddles") {
        SequenceTracker t(SequencePolicy::kBinanceSpot);
        t.on_snapshot(100);
        CHECK(t.on_update(spot(100, 105)) == SequenceAction::kApply);
    }
    SECTION("U exactly one past the snapshot id straddles") {
        // The modal case, and the one a futures-shaped rule rejects. After any
        // resync the buffer is dropped and the very next event begins at
        // lastUpdateId + 1 — so if this resyncs, it resyncs forever on a
        // stream that never gapped.
        SequenceTracker t(SequencePolicy::kBinanceSpot);
        t.on_snapshot(100);
        CHECK(t.on_update(spot(101, 105)) == SequenceAction::kApply);
        CHECK(t.straddled());
        CHECK(t.stats().gaps_detected == 0);
    }
    SECTION("u exactly at the snapshot id is fully contained by it") {
        // Spot discards where u <= lastUpdateId: every change this event
        // carries is already in the snapshot. Futures, anchoring one lower,
        // treats the same event as the straddle. See the divergence test below.
        SequenceTracker t(SequencePolicy::kBinanceSpot);
        t.on_snapshot(100);
        CHECK(t.on_update(spot(95, 100)) == SequenceAction::kDiscardStale);
        CHECK(t.stats().gaps_detected == 0);
        CHECK(t.synced());
    }
    SECTION("a snapshot older than the stream forces a resync") {
        // The book we fetched is already behind what the socket is delivering,
        // so there is a hole between them that no amount of applying fixes.
        SequenceTracker t(SequencePolicy::kBinanceSpot);
        t.on_snapshot(100);
        CHECK(t.on_update(spot(105, 110)) == SequenceAction::kResyncRequired);
        CHECK(t.stats().gaps_detected == 1);
        CHECK_FALSE(t.synced());
    }
}

TEST_CASE("spot enforces U == previous u + 1", "[sequence][spot]") {
    SequenceTracker t(SequencePolicy::kBinanceSpot);
    t.on_snapshot(100);
    REQUIRE(t.on_update(spot(98, 105)) == SequenceAction::kApply);

    SECTION("contiguous events are applied") {
        CHECK(t.on_update(spot(106, 110)) == SequenceAction::kApply);
        CHECK(t.on_update(spot(111, 111)) == SequenceAction::kApply);
        CHECK(t.stats().applied == 3);
        CHECK(t.stats().gaps_detected == 0);
    }
    SECTION("a dropped update is caught") {
        CHECK(t.on_update(spot(108, 110)) == SequenceAction::kResyncRequired);
        CHECK(t.stats().gaps_detected == 1);
        CHECK_FALSE(t.synced());
    }
    SECTION("an off-by-one gap is caught") {
        // The single most likely real failure, and the one a naive
        // implementation misses entirely.
        CHECK(t.on_update(spot(107, 110)) == SequenceAction::kResyncRequired);
    }
    SECTION("a duplicate delivery is discarded, not treated as a gap") {
        CHECK(t.on_update(spot(98, 105)) == SequenceAction::kDiscardStale);
        CHECK(t.stats().gaps_detected == 0);
        CHECK(t.synced());
    }
}

// ---------------------------------------------------------------------------
// Binance futures: pu == previous u
// ---------------------------------------------------------------------------

TEST_CASE("futures enforces pu == previous u", "[sequence][futures]") {
    SequenceTracker t(SequencePolicy::kBinanceFutures);
    t.on_snapshot(100);
    REQUIRE(t.on_update(futures(98, 105, 97)) == SequenceAction::kApply);

    SECTION("matching pu is applied") {
        CHECK(t.on_update(futures(106, 110, 105)) == SequenceAction::kApply);
        CHECK(t.on_update(futures(111, 120, 110)) == SequenceAction::kApply);
    }
    SECTION("mismatched pu forces a resync") {
        CHECK(t.on_update(futures(106, 110, 104)) == SequenceAction::kResyncRequired);
        CHECK(t.stats().gaps_detected == 1);
    }
    SECTION("futures does NOT require U == previous u + 1") {
        // The distinction that matters: on futures, U may legitimately not be
        // contiguous. A handler that applies the spot rule here would report
        // constant phantom gaps.
        CHECK(t.on_update(futures(999, 110, 105)) == SequenceAction::kApply);
    }
}

TEST_CASE("the spot rule and the futures rule genuinely differ", "[sequence]") {
    // Same wire event, opposite verdicts. This is why the policy is explicit.
    const UpdateIds event = futures(999, 110, 105);

    SequenceTracker spot_tracker(SequencePolicy::kBinanceSpot);
    spot_tracker.on_snapshot(100);
    REQUIRE(spot_tracker.on_update(spot(98, 105)) == SequenceAction::kApply);
    CHECK(spot_tracker.on_update(event) == SequenceAction::kResyncRequired);

    SequenceTracker futures_tracker(SequencePolicy::kBinanceFutures);
    futures_tracker.on_snapshot(100);
    REQUIRE(futures_tracker.on_update(futures(98, 105, 97)) == SequenceAction::kApply);
    CHECK(futures_tracker.on_update(event) == SequenceAction::kApply);
}

TEST_CASE("spot and futures anchor snapshot reconciliation differently", "[sequence]") {
    // The two venues do not merely differ in ongoing continuity — they differ
    // in where reconciliation begins. Spot anchors at lastUpdateId + 1 and
    // discards anything at or below the snapshot; futures anchors on
    // lastUpdateId itself. One constant, and getting it wrong on spot means
    // every resync demands another resync.
    const UpdateIds contained = UpdateIds{95, 100, 94};   // u == lastUpdateId
    const UpdateIds resumption = UpdateIds{101, 105, 100};  // U == lastUpdateId + 1

    SECTION("an event ending exactly at the snapshot id") {
        SequenceTracker spot_tracker(SequencePolicy::kBinanceSpot);
        spot_tracker.on_snapshot(100);
        CHECK(spot_tracker.on_update(contained) == SequenceAction::kDiscardStale);

        SequenceTracker futures_tracker(SequencePolicy::kBinanceFutures);
        futures_tracker.on_snapshot(100);
        CHECK(futures_tracker.on_update(contained) == SequenceAction::kApply);
    }

    SECTION("an event resuming exactly one past the snapshot id") {
        // Spot applies it: 101 is exactly spot's anchor.
        SequenceTracker spot_tracker(SequencePolicy::kBinanceSpot);
        spot_tracker.on_snapshot(100);
        CHECK(spot_tracker.on_update(resumption) == SequenceAction::kApply);

        // Futures does NOT, and that is the documented rule rather than an
        // oversight: its procedure requires U <= lastUpdateId <= u, so an
        // event beginning past the snapshot means the snapshot was fetched
        // without a straddling event buffered and must be refetched. The
        // asymmetry is the whole reason the anchor is policy-dependent — the
        // bug was applying THIS behaviour to spot, where the same event is the
        // normal resumption.
        SequenceTracker futures_tracker(SequencePolicy::kBinanceFutures);
        futures_tracker.on_snapshot(100);
        CHECK(futures_tracker.on_update(resumption) == SequenceAction::kResyncRequired);
    }

    SECTION("a spot resync recovers instead of looping") {
        // The regression this test exists for: drop the buffer, refetch, and
        // the next event necessarily starts at lastUpdateId + 1. Under a
        // futures-shaped anchor that reads as a gap, and the recovery never
        // converges.
        SequenceTracker t(SequencePolicy::kBinanceSpot);
        t.on_snapshot(100);
        REQUIRE(t.on_update(spot(105, 110)) == SequenceAction::kResyncRequired);
        REQUIRE_FALSE(t.synced());

        t.on_snapshot(200);
        CHECK(t.on_update(spot(201, 210)) == SequenceAction::kApply);
        CHECK(t.on_update(spot(211, 215)) == SequenceAction::kApply);
        CHECK(t.synced());
        CHECK(t.stats().gaps_detected == 1);
    }
}

// ---------------------------------------------------------------------------
// Strict increment (Coinbase full)
// ---------------------------------------------------------------------------

TEST_CASE("strict increment requires consecutive sequence numbers", "[sequence][coinbase]") {
    SequenceTracker t(SequencePolicy::kStrictIncrement);
    t.on_snapshot(1000);

    SECTION("consecutive is applied") {
        CHECK(t.on_update(strict(1001)) == SequenceAction::kApply);
        CHECK(t.on_update(strict(1002)) == SequenceAction::kApply);
        CHECK(t.on_update(strict(1003)) == SequenceAction::kApply);
        CHECK(t.stats().applied == 3);
    }
    SECTION("a hole forces a resync") {
        CHECK(t.on_update(strict(1002)) == SequenceAction::kResyncRequired);
        CHECK(t.stats().gaps_detected == 1);
    }
    SECTION("messages at or below the snapshot are stale") {
        CHECK(t.on_update(strict(1000)) == SequenceAction::kDiscardStale);
        CHECK(t.on_update(strict(999)) == SequenceAction::kDiscardStale);
        CHECK(t.synced());
    }
    SECTION("replays mid-stream are discarded, not gaps") {
        REQUIRE(t.on_update(strict(1001)) == SequenceAction::kApply);
        REQUIRE(t.on_update(strict(1002)) == SequenceAction::kApply);
        CHECK(t.on_update(strict(1002)) == SequenceAction::kDiscardStale);
        CHECK(t.on_update(strict(1001)) == SequenceAction::kDiscardStale);
        CHECK(t.stats().gaps_detected == 0);
        CHECK(t.on_update(strict(1003)) == SequenceAction::kApply);
    }
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

TEST_CASE("a gap makes every later update unsafe until resnapshot", "[sequence]") {
    // The dangerous failure is not missing the gap; it is noticing the gap and
    // then continuing to apply updates anyway.
    SequenceTracker t(SequencePolicy::kBinanceSpot);
    t.on_snapshot(100);
    REQUIRE(t.on_update(spot(98, 105)) == SequenceAction::kApply);
    REQUIRE(t.on_update(spot(108, 110)) == SequenceAction::kResyncRequired);

    CHECK(t.on_update(spot(111, 112)) == SequenceAction::kResyncRequired);
    CHECK(t.on_update(spot(113, 114)) == SequenceAction::kResyncRequired);
    CHECK_FALSE(t.synced());
}

TEST_CASE("a fresh snapshot restores the stream", "[sequence]") {
    SequenceTracker t(SequencePolicy::kBinanceSpot);
    t.on_snapshot(100);
    REQUIRE(t.on_update(spot(98, 105)) == SequenceAction::kApply);
    REQUIRE(t.on_update(spot(108, 110)) == SequenceAction::kResyncRequired);

    t.on_snapshot(200);
    CHECK(t.synced());
    CHECK_FALSE(t.straddled());
    CHECK(t.on_update(spot(198, 205)) == SequenceAction::kApply);
    CHECK(t.stats().snapshots_applied == 2);
}

TEST_CASE("invalidate forces a resync", "[sequence]") {
    // Used on socket reconnect and on staleness timeout, where the sequence
    // numbers look fine but the connection underneath them did not.
    SequenceTracker t(SequencePolicy::kBinanceSpot);
    t.on_snapshot(100);
    REQUIRE(t.on_update(spot(98, 105)) == SequenceAction::kApply);

    t.invalidate();
    CHECK_FALSE(t.synced());
    CHECK(t.on_update(spot(106, 110)) == SequenceAction::kResyncRequired);
}

TEST_CASE("stats account for every update", "[sequence]") {
    // The health endpoint publishes these, so they need to add up.
    SequenceTracker t(SequencePolicy::kBinanceSpot);
    t.on_snapshot(100);
    (void)t.on_update(spot(90, 95));    // stale
    (void)t.on_update(spot(98, 105));   // applied
    (void)t.on_update(spot(106, 110));  // applied
    (void)t.on_update(spot(120, 130));  // gap

    CHECK(t.stats().applied == 2);
    CHECK(t.stats().discarded_stale == 1);
    CHECK(t.stats().gaps_detected == 1);
    CHECK(t.stats().snapshots_applied == 1);
}
