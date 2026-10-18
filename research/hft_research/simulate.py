"""Synthetic L2 market for tests, benchmarks and demos, with no exchange needed.

The mid follows a random walk with a small *planted* dependence on order-book
imbalance, so a model has something real to learn and the backtest can show
that learning it is worth more than chance. Real data needs the gateway's
recorder (`gateway -record ...`).

    python -m hft_research.simulate --events 2000000 --out data/synthetic.bin
"""

from __future__ import annotations

import argparse

import numpy as np

from . import events as ev


def simulate(n_events: int, seed: int = 7, levels: int = 10, start_mid: int = 6_500_000) -> np.ndarray:
    rng = np.random.default_rng(seed)
    out = np.zeros(n_events + 2 * levels + 2, dtype=ev.EVENT_DTYPE)
    i = 0
    t = 1_700_000_000_000_000_000
    mid = start_mid  # ticks; tick = 0.01 -> $65,000.00
    bids = {mid - k: int(rng.integers(5, 50)) for k in range(1, levels + 1)}
    asks = {mid + k: int(rng.integers(5, 50)) for k in range(1, levels + 1)}

    def put(typ, side=0, price=0, qty=0):
        nonlocal i
        out[i] = (typ, side, 0, 0, t, t, price, qty, i, b"\0" * 16)
        i += 1

    put(ev.SNAPSHOT_BEGIN)
    for p, q in bids.items():
        put(ev.BOOK_LEVEL, ev.BID, p, q)
    for p, q in asks.items():
        put(ev.BOOK_LEVEL, ev.ASK, p, q)
    put(ev.SNAPSHOT_END)

    while i < len(out):
        t += int(rng.exponential(200_000))  # ~5k events/s of market time
        best_bid, best_ask = max(bids), min(asks)
        imb = (bids[best_bid] - asks[best_ask]) / (bids[best_bid] + asks[best_ask])
        r = rng.random()
        if r < 0.08:  # trade, more likely on the heavy side
            buy = rng.random() < 0.5 + 0.3 * imb
            put(ev.TRADE, ev.AGG_BUY if buy else ev.AGG_SELL, best_ask if buy else best_bid, int(rng.integers(1, 10)))
        elif r < 0.10:  # mid moves one tick, biased by imbalance (the planted signal)
            up = rng.random() < 0.5 + 0.25 * imb
            shift = 1 if up else -1
            if up:
                put(ev.BOOK_LEVEL, ev.ASK, best_ask, 0)
                asks.pop(best_ask)
                nb = best_ask
                bids[nb] = int(rng.integers(5, 50))
                put(ev.BOOK_LEVEL, ev.BID, nb, bids[nb])
                asks[max(asks) + 1] = int(rng.integers(5, 50))
                bids.pop(min(bids))
            else:
                put(ev.BOOK_LEVEL, ev.BID, best_bid, 0)
                bids.pop(best_bid)
                na = best_bid
                asks[na] = int(rng.integers(5, 50))
                put(ev.BOOK_LEVEL, ev.ASK, na, asks[na])
                bids[min(bids) - 1] = int(rng.integers(5, 50))
                asks.pop(max(asks))
            mid += shift
        else:  # size change somewhere in the top levels
            side = ev.BID if rng.random() < 0.5 else ev.ASK
            book = bids if side == ev.BID else asks
            prices = sorted(book, reverse=(side == ev.BID))[:5]
            p = prices[int(rng.integers(0, len(prices)))]
            book[p] = max(1, book[p] + int(rng.integers(-8, 9)))
            put(ev.BOOK_LEVEL, side, p, book[p])
    return out[:i]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--events", type=int, default=2_000_000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", default="data/synthetic.bin")
    a = ap.parse_args()
    arr = simulate(a.events, a.seed)
    ev.write(a.out, arr)
    print(f"wrote {len(arr):,} events to {a.out}")


if __name__ == "__main__":
    main()
