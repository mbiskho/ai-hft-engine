"""INT8 quantization of the signal model, with an accuracy and latency check.

    python -m hft_research.quantize --model models/signal.onnx --data data/synthetic.bin

Dynamic INT8 shrinks the weights 4x and usually speeds up CPU inference. The
script refuses to bless the quantized model if its predictions drift more
than --max-drift from FP32 on real feature vectors: a faster model that
signals differently is a different strategy.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
from onnxruntime.quantization import QuantType, quantize_dynamic

from . import events as ev
from .dataset import build


def session(path: Path) -> ort.InferenceSession:
    opt = ort.SessionOptions()
    opt.intra_op_num_threads = 1
    opt.inter_op_num_threads = 1
    return ort.InferenceSession(str(path), opt, providers=["CPUExecutionProvider"])


def bench(sess: ort.InferenceSession, X: np.ndarray, n: int = 20000) -> dict[str, float]:
    x = X[:1].copy()
    for _ in range(500):
        sess.run(["p_up"], {"features": x})
    t = np.empty(n)
    for i in range(n):
        x[0] = X[i % len(X)]
        s = time.perf_counter_ns()
        sess.run(["p_up"], {"features": x})
        t[i] = time.perf_counter_ns() - s
    return {"p50_us": float(np.percentile(t, 50) / 1e3), "p99_us": float(np.percentile(t, 99) / 1e3)}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="models/signal.onnx")
    ap.add_argument("--data", required=True)
    ap.add_argument("--max-drift", type=float, default=0.02)
    a = ap.parse_args()
    src = Path(a.model)
    dst = src.with_name(src.stem + ".int8.onnx")
    quantize_dynamic(str(src), str(dst), weight_type=QuantType.QInt8)

    X, _ = build(ev.read(a.data))
    X = X[-20000:]
    fp32, int8 = session(src), session(dst)
    p32 = np.array([fp32.run(["p_up"], {"features": x[None]})[0][0, 0] for x in X])
    p8 = np.array([int8.run(["p_up"], {"features": x[None]})[0][0, 0] for x in X])
    drift = float(np.abs(p32 - p8).max())
    agree = float(((p32 > 0.5) == (p8 > 0.5)).mean())

    report = {
        "fp32": {"bytes": src.stat().st_size, **bench(fp32, X)},
        "int8": {"bytes": dst.stat().st_size, **bench(int8, X)},
        "max_abs_drift": drift,
        "decision_agreement": agree,
        "accepted": drift <= a.max_drift,
    }
    Path(src.with_name("quantization.json")).write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    if not report["accepted"]:
        raise SystemExit(f"INT8 drift {drift:.4f} exceeds {a.max_drift}; keep the FP32 model")


if __name__ == "__main__":
    main()
