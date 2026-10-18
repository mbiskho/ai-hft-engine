"""Read and write the 64-byte MarketEvent format (shared/event_format.md)."""

from __future__ import annotations

from pathlib import Path

import numpy as np

BOOK_LEVEL, TRADE, SNAPSHOT_BEGIN, SNAPSHOT_END, KILL_SWITCH, HEARTBEAT = 1, 2, 3, 4, 5, 6
BID, ASK = 0, 1
AGG_BUY, AGG_SELL = 0, 1

EVENT_DTYPE = np.dtype(
    [
        ("type", "<u1"),
        ("side", "<u1"),
        ("symbol_id", "<u2"),
        ("flags", "<u4"),
        ("exchange_ts_ns", "<u8"),
        ("recv_ts_ns", "<u8"),
        ("price", "<i8"),
        ("qty", "<i8"),
        ("seq", "<u8"),
        ("reserved", "V16"),
    ]
)
assert EVENT_DTYPE.itemsize == 64, "must match the C++ static_assert"


def read(path: str | Path) -> np.ndarray:
    return np.fromfile(path, dtype=EVENT_DTYPE)


def write(path: str | Path, events: np.ndarray) -> None:
    events.astype(EVENT_DTYPE, copy=False).tofile(path)
