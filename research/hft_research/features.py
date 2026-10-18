"""Reference implementation of the engine's features.

Same definitions as engine/include/hft/features.hpp. Pure Python, no numpy, so
the parity test runs anywhere and the formulas read as math. dataset.py calls
these while replaying recordings to build training data. If the two
implementations drift, the model is trained on features the engine never
computes, and shared/feature_vectors.csv exists to catch that.
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field

NAMES = ["spread_bps", "imb1", "imb5", "micro_dev_bps", "ret10_bps", "ret50_bps", "trade_imb", "depth_ratio"]


def static_features(bids: list[tuple[int, int]], asks: list[tuple[int, int]]) -> dict[str, float]:
    """Features that depend only on the current book. Levels are (price, qty), best first."""
    (bid, bq), (ask, aq) = bids[0], asks[0]
    mid = (bid + ask) / 2
    sb = sum(q for _, q in bids[:5])
    sa = sum(q for _, q in asks[:5])
    micro = (bid * aq + ask * bq) / (bq + aq)
    return {
        "spread_bps": (ask - bid) / mid * 1e4,
        "imb1": (bq - aq) / (bq + aq),
        "imb5": (sb - sa) / (sb + sa),
        "micro_dev_bps": (micro - mid) / mid * 1e4,
        "depth_ratio": math.log(sb / sa),
    }


@dataclass
class FeatureState:
    mids: deque = field(default_factory=lambda: deque(maxlen=64))
    trades: deque = field(default_factory=lambda: deque(maxlen=64))

    def on_book(self, mid: float) -> None:
        self.mids.append(mid)

    def on_trade(self, aggressor: int, qty: int) -> None:
        self.trades.append(qty if aggressor == 0 else -qty)

    def _ret(self, mid: float, back: int) -> float:
        if len(self.mids) <= back:
            return 0.0
        return math.log(mid / self.mids[-1 - back]) * 1e4

    def compute(self, bids, asks) -> list[float]:
        f = static_features(bids, asks)
        mid = (bids[0][0] + asks[0][0]) / 2
        total = sum(abs(t) for t in self.trades)
        f["ret10_bps"] = self._ret(mid, 10)
        f["ret50_bps"] = self._ret(mid, 50)
        f["trade_imb"] = sum(self.trades) / total if total else 0.0
        return [f[n] for n in NAMES]
