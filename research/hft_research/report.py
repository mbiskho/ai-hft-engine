"""Backtest report: equity curve, position, and latency from an engine run.

    hft-engine --input data/synthetic.bin --model onnx:models/signal.int8.onnx \
               --config config/engine.conf --fills reports/fills.csv > reports/backtest.json
    python -m hft_research.report --stats reports/backtest.json --fills reports/fills.csv --out reports/
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stats", required=True)
    ap.add_argument("--fills", required=True)
    ap.add_argument("--out", default="reports")
    a = ap.parse_args()
    stats = json.loads(Path(a.stats).read_text())
    rows = list(csv.DictReader(open(a.fills)))
    out = Path(a.out)

    t0 = int(rows[0]["market_ns"]) if rows else 0
    mins = [(int(r["market_ns"]) - t0) / 6e10 for r in rows]
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 6), sharex=True, height_ratios=[2, 1])
    ax1.plot(mins, [float(r["pnl"]) for r in rows], lw=1.4)
    ax1.set_ylabel("PnL (ticks × lots, net of fees)")
    ax2.step(mins, [int(r["position_after"]) for r in rows], where="post", lw=1)
    ax2.axhline(stats["position_limit"], ls="--", lw=0.8, c="r")
    ax2.axhline(-stats["position_limit"], ls="--", lw=0.8, c="r")
    ax2.set_ylabel("position (lots)")
    ax2.set_xlabel("market minutes")
    fig.tight_layout()
    fig.savefig(out / "backtest-equity.png", dpi=150)

    summary = {
        "events_per_sec": stats["events_per_sec"],
        "signal_p50_us": stats["signal_ns"]["p50"] / 1e3,
        "signal_p99_us": stats["signal_ns"]["p99"] / 1e3,
        "fills": stats["fills"],
        "pnl": stats["pnl_ticks_lots"],
        "max_drawdown": stats["max_drawdown"],
        "max_abs_position_within_limit": stats["max_abs_position"] <= stats["position_limit"],
    }
    (out / "backtest-summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
