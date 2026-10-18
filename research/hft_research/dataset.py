"""Replay a recording through the reference book and features to build (X, y).

Label: does the mid rise over the next `horizon` book events? (Ties dropped:
most windows at this horizon don't move, and a "no move" class swamps both.)
Samples are taken at most every `stride` events, so neighbouring rows aren't
near-duplicates that inflate validation accuracy.
"""

from __future__ import annotations

import numpy as np

from . import events as ev
from .features import FeatureState


def build(events: np.ndarray, horizon: int = 50, stride: int = 10) -> tuple[np.ndarray, np.ndarray]:
    bids: dict[int, int] = {}
    asks: dict[int, int] = {}
    fs = FeatureState()
    feats: list[list[float]] = []
    mids: list[float] = []
    n_book = 0
    for e in events:
        typ = int(e["type"])
        if typ == ev.SNAPSHOT_BEGIN:
            bids.clear()
            asks.clear()
        elif typ == ev.TRADE:
            fs.on_trade(int(e["side"]), int(e["qty"]))
        elif typ == ev.BOOK_LEVEL:
            book = bids if int(e["side"]) == ev.BID else asks
            p, q = int(e["price"]), int(e["qty"])
            if q == 0:
                book.pop(p, None)
            else:
                book[p] = q
            if not bids or not asks:
                continue
            b = sorted(bids.items(), reverse=True)[:5]
            a = sorted(asks.items())[:5]
            if b[0][0] >= a[0][0]:
                continue
            mid = (b[0][0] + a[0][0]) / 2
            fs.on_book(mid)
            mids.append(mid)
            n_book += 1
            feats.append(fs.compute(b, a) if n_book % stride == 0 else None)

    X, y = [], []
    for i, f in enumerate(feats):
        if f is None or i + horizon >= len(mids):
            continue
        move = mids[i + horizon] - mids[i]
        if move == 0:
            continue
        X.append(f)
        y.append(1.0 if move > 0 else 0.0)
    return np.asarray(X, dtype=np.float32), np.asarray(y, dtype=np.float32)
