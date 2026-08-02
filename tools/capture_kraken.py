#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Josh Dardashti
"""Capture Kraken WebSocket v2 book frames to a .cbcap file.

This is a development utility, not part of the library. It exists because
crossbook deliberately has no socket layer (see the README): the correctness
core stays zero-dependency and testable offline, and transport is whatever you
already have. Python is a perfectly good "whatever you already have" for
producing a capture.

The output feeds `crossbook_verify`, which replays it through the real decoder
and book and reports the match rate against Kraken's own CRC32 -- the claim the
whole library rests on.

Usage:
    python tools/capture_kraken.py --symbol BTC/USD --seconds 600 --out cap.cbcap

Format (little-endian), mirrored by include/crossbook/capture.hpp:
    magic   8 bytes  "CBCAP1\\0\\0"
    record  int64 ts_recv_ns, uint32 length, length bytes of payload
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import time
from pathlib import Path

try:
    import websockets
except ImportError:  # pragma: no cover - dev utility
    sys.exit("pip install websockets")

MAGIC = b"CBCAP1\0\0"
KRAKEN_WS_V2 = "wss://ws.kraken.com/v2"


async def capture(symbol: str, depth: int, seconds: float, out: Path) -> tuple[int, int]:
    """Stream book frames for `seconds`, returning (frames, bytes)."""
    subscribe = {
        "method": "subscribe",
        "params": {"channel": "book", "symbol": [symbol], "depth": depth, "snapshot": True},
    }

    frames = 0
    payload_bytes = 0
    deadline = time.monotonic() + seconds

    with out.open("wb") as sink:
        sink.write(MAGIC)
        # ping_interval keeps the socket alive without us writing a keepalive;
        # Kraken also sends its own heartbeats, which are captured verbatim so
        # the replay sees exactly what the wire carried.
        async with websockets.connect(KRAKEN_WS_V2, ping_interval=20, max_size=None) as ws:
            import json

            await ws.send(json.dumps(subscribe))
            while time.monotonic() < deadline:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    message = await asyncio.wait_for(ws.recv(), timeout=min(remaining, 30.0))
                except asyncio.TimeoutError:
                    continue

                # Local receive time. Venue clocks are not comparable across
                # venues; local time is, and it is what the replay harness
                # paces against.
                ts_ns = time.time_ns()
                data = message.encode("utf-8") if isinstance(message, str) else message
                sink.write(struct.pack("<qI", ts_ns, len(data)))
                sink.write(data)
                frames += 1
                payload_bytes += len(data)

                if frames % 500 == 0:
                    print(f"  {frames} frames, {payload_bytes/1024:.0f} KiB", flush=True)

    return frames, payload_bytes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--symbol", default="BTC/USD")
    parser.add_argument("--depth", type=int, default=25, choices=[10, 25, 100, 500, 1000])
    parser.add_argument("--seconds", type=float, default=600.0)
    parser.add_argument("--out", type=Path, default=Path("kraken.cbcap"))
    args = parser.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    print(f"capturing {args.symbol} depth={args.depth} for {args.seconds:.0f}s -> {args.out}")

    frames, payload = asyncio.run(capture(args.symbol, args.depth, args.seconds, args.out))
    print(f"done: {frames} frames, {payload/1024:.0f} KiB payload, file {args.out.stat().st_size} bytes")
    return 0 if frames > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
