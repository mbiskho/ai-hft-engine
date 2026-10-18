"""Parity with the C++ engine via shared/feature_vectors.csv (the same file engine/tests reads)."""

from pathlib import Path

import pytest

from hft_research.features import FeatureState, static_features

CSV = Path(__file__).resolve().parents[2] / "shared" / "feature_vectors.csv"


def cases():
    for line in CSV.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        c = line.split(",")
        ints = lambda s: [int(x) for x in s.split(";")]  # noqa: E731
        yield c[0], list(zip(ints(c[1]), ints(c[2]), strict=True)), list(zip(ints(c[3]), ints(c[4]), strict=True)), [float(x) for x in c[5:]]


@pytest.mark.parametrize("name,bids,asks,expected", list(cases()))
def test_static_features_match_shared_vectors(name, bids, asks, expected):
    f = static_features(bids, asks)
    got = [f["spread_bps"], f["imb1"], f["imb5"], f["micro_dev_bps"], f["depth_ratio"]]
    assert got == pytest.approx(expected, abs=1e-9), name


def test_history_features():
    fs = FeatureState()
    for _ in range(10):
        fs.on_book(10001.0)
    fs.on_book(10002.0)
    fs.on_trade(0, 3)
    fs.on_trade(1, 1)
    x = fs.compute([(10000, 1)], [(10004, 1)])
    assert x[4] == pytest.approx(1.0e4 * __import__("math").log(10002 / 10001))
    assert x[5] == 0.0
    assert x[6] == pytest.approx(0.5)
