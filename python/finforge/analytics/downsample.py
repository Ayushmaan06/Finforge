"""Downsampling long series for display (PLAN Phase 8 task 4).

Two methods. Min/max per bucket (the default) keeps each bucket's lowest and highest point; it is
a single vectorised pass, so a one-year minute series (525 600 points) reduces in a few
milliseconds, and no extreme is ever lost. LTTB (Largest-Triangle-Three-Buckets, Steinarsson 2013)
keeps the points that matter visually: the first and last points, and in each bucket the point
forming the largest triangle with the point kept before it and the average of the next bucket;
it usually keeps the global extremes but does not guarantee them.
`keep` forces extra indices in (e.g. the maximum drawdown), so a metric shown next to a chart is
always visible on it.
"""

import numpy as np


def lttb(x: np.ndarray, y: np.ndarray, n: int) -> np.ndarray:
    """Indices (sorted, unique) of at most `n` points of the series (x increasing)."""
    m = len(x)
    if n >= m or m <= 2:
        return np.arange(m)
    if n < 3:
        return np.array([0, m - 1])
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    # n - 2 buckets between the fixed first and last points.
    edges = np.linspace(1, m - 1, n - 1).astype(np.int64)
    out = np.empty(n, dtype=np.int64)
    out[0], out[-1] = 0, m - 1
    a = 0
    for i in range(n - 2):
        lo, hi = edges[i], max(edges[i + 1], edges[i] + 1)
        if i + 2 < len(edges):
            nlo, nhi = edges[i + 1], max(edges[i + 2], edges[i + 1] + 1)
        else:
            nlo, nhi = m - 1, m
        avg_x, avg_y = x[nlo:nhi].mean(), y[nlo:nhi].mean()
        bx, by = x[lo:hi], y[lo:hi]
        # Twice the triangle area (sign irrelevant).
        area = np.abs((x[a] - avg_x) * (by - y[a]) - (x[a] - bx) * (avg_y - y[a]))
        a = lo + int(np.argmax(area))
        out[i + 1] = a
    return np.unique(out)


def minmax(y: np.ndarray, n: int) -> np.ndarray:
    """Indices of the minimum and maximum of each of n/2 equal buckets, plus the first and last
    points: at most n + 2 points. Fully vectorised (one reshape), and every bucket's extremes are
    kept exactly, so no peak or trough is ever lost."""
    m = len(y)
    if n >= m or m <= 2:
        return np.arange(m)
    buckets = max(1, n // 2)
    size = -(-m // buckets)  # ceil
    padded = np.empty(buckets * size, dtype=np.float64)
    padded[:m] = y
    padded[m:] = y[-1]  # padding repeats the last value; its indices are clipped below
    grid = padded.reshape(buckets, size)
    base = np.arange(buckets) * size
    idx = np.concatenate([[0, m - 1], base + grid.argmin(axis=1), base + grid.argmax(axis=1)])
    return np.unique(np.minimum(idx, m - 1))


def downsample(
    x: np.ndarray, y: np.ndarray, n: int, keep: list[int] | None = None, method: str = "minmax"
) -> np.ndarray:
    """Indices of at most ~n points ("minmax", the default, or "lttb") plus any in `keep`."""
    idx = minmax(np.asarray(y, dtype=np.float64), n) if method == "minmax" else lttb(x, y, n)
    if keep:
        idx = np.unique(np.concatenate([idx, np.asarray(keep, dtype=np.int64)]))
    return idx
