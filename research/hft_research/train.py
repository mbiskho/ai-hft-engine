"""Train the signal model and export it for the engine.

    python -m hft_research.train --data data/synthetic.bin --out models/

Writes:
  models/signal.onnx    MLP 8 -> 32 -> 16 -> 1 with the sigmoid inside;
                        input "features" [1, 8] float32, output "p_up" [1, 1]
  models/linear.txt     logistic-regression fallback (8 weights + bias) for builds without ONNX Runtime
  models/metrics.json   validation AUC / accuracy for both, on a time-ordered split

The split is chronological (first 80% train, last 20% validate), never
shuffled. Shuffling time series leaks the future into training and produces
validation numbers you will never see live.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import torch
from torch import nn

from . import events as ev
from .dataset import build


class Signal(nn.Module):
    def __init__(self, mean: np.ndarray, std: np.ndarray):
        super().__init__()
        # Normalisation baked into the graph: the engine feeds raw features.
        self.register_buffer("mean", torch.tensor(mean, dtype=torch.float32))
        self.register_buffer("std", torch.tensor(std, dtype=torch.float32))
        self.net = nn.Sequential(nn.Linear(8, 32), nn.ReLU(), nn.Linear(32, 16), nn.ReLU(), nn.Linear(16, 1))

    def forward(self, x):
        return torch.sigmoid(self.net((x - self.mean) / self.std))


def auc(y: np.ndarray, p: np.ndarray) -> float:
    order = np.argsort(p)
    ranks = np.empty_like(order, dtype=np.float64)
    ranks[order] = np.arange(1, len(p) + 1)
    pos = y == 1
    n_pos, n_neg = pos.sum(), (~pos).sum()
    return float((ranks[pos].sum() - n_pos * (n_pos + 1) / 2) / (n_pos * n_neg))


def fit_logistic(X: np.ndarray, y: np.ndarray, epochs: int = 300, lr: float = 0.1) -> tuple[np.ndarray, float]:
    mean, std = X.mean(0), X.std(0) + 1e-8
    Z = (X - mean) / std
    w, b = np.zeros(X.shape[1]), 0.0
    for _ in range(epochs):
        p = 1 / (1 + np.exp(-(Z @ w + b)))
        g = p - y
        w -= lr * (Z.T @ g) / len(y)
        b -= lr * g.mean()
    # Fold the normalisation into raw-feature weights so the engine needs no scaler.
    return w / std, b - float((w * mean / std).sum())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", default="models")
    ap.add_argument("--epochs", type=int, default=15)
    ap.add_argument("--horizon", type=int, default=50)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    X, y = build(ev.read(a.data), horizon=a.horizon)
    cut = int(0.8 * len(X))
    Xtr, ytr, Xva, yva = X[:cut], y[:cut], X[cut:], y[cut:]
    print(f"samples train={len(Xtr):,} val={len(Xva):,} positive rate={y.mean():.3f}")

    torch.manual_seed(0)
    model = Signal(Xtr.mean(0), Xtr.std(0) + 1e-8)
    opt = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=1e-4)
    loss_fn = nn.BCELoss()
    xt, yt = torch.from_numpy(Xtr), torch.from_numpy(ytr).unsqueeze(1)
    for epoch in range(a.epochs):
        perm = torch.randperm(len(xt))  # shuffling *within* the training window is fine
        for i in range(0, len(xt), 1024):
            idx = perm[i : i + 1024]
            opt.zero_grad()
            loss = loss_fn(model(xt[idx]), yt[idx])
            loss.backward()
            opt.step()
        with torch.no_grad():
            pv = model(torch.from_numpy(Xva)).squeeze(1).numpy()
        print(f"epoch {epoch + 1:2d}  val AUC {auc(yva, pv):.4f}  acc {((pv > 0.5) == yva).mean():.4f}")

    model.eval()
    torch.onnx.export(
        model, torch.zeros(1, 8), out / "signal.onnx", input_names=["features"], output_names=["p_up"], opset_version=17,
    )

    w, b = fit_logistic(Xtr, ytr)
    (out / "linear.txt").write_text(" ".join(f"{v:.8g}" for v in w) + f" {b:.8g}\n")
    pl = 1 / (1 + np.exp(-(Xva @ w + b)))

    metrics = {
        "samples": {"train": int(len(Xtr)), "val": int(len(Xva))},
        "mlp": {"val_auc": auc(yva, pv), "val_acc": float(((pv > 0.5) == yva).mean())},
        "linear": {"val_auc": auc(yva, pl), "val_acc": float(((pl > 0.5) == yva).mean())},
    }
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2))
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
