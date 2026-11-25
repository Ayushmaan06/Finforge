"""Display downsampling (PLAN Phase 8 task 4)."""

import numpy as np
import pytest

from finforge.analytics.downsample import downsample, lttb, minmax


@pytest.mark.parametrize("method", ["minmax", "lttb"])
def test_keeps_ends_and_extremes_and_bounds_size(method: str) -> None:
    rng = np.random.default_rng(3)
    y = np.cumsum(rng.normal(size=100_000))
    x = np.arange(len(y), dtype=float)
    idx = downsample(x, y, 1000, method=method)
    assert idx[0] == 0 and idx[-1] == len(y) - 1
    assert np.all(np.diff(idx) > 0)  # sorted, unique: valid as chart data
    assert len(idx) <= 1002
    if method == "minmax":  # guaranteed by construction; LTTB usually keeps them but need not
        assert y[idx].max() == y.max() and y[idx].min() == y.min()
    # Forced points are always there: this is how the API keeps the max-drawdown point.
    lo = int(np.argmin(y))
    assert lo in downsample(x, y, 1000, keep=[lo], method=method)


def test_minmax_keeps_every_bucket_extreme() -> None:
    y = np.array([5, 1, 9, 2, 8, 3, 7, 0, 6, 4], dtype=float)
    idx = minmax(y, 4)  # 2 buckets of 5: [5 1 9 2 8] and [3 7 0 6 4]
    assert set(idx.tolist()) == {0, 9, 1, 2, 7, 6}


def test_short_series_and_forced_points() -> None:
    y = np.array([1.0, 2.0, 3.0])
    assert minmax(y, 10).tolist() == [0, 1, 2]
    assert lttb(np.arange(3.0), y, 10).tolist() == [0, 1, 2]
    y = np.arange(1000, dtype=float)
    idx = downsample(np.arange(1000.0), y, 10, keep=[123, 456])
    assert 123 in idx and 456 in idx
