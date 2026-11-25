"""Validation codes, policies and the structured report (PLAN Phase 3 task 4).

Validation never stops at the first problem. Every problem is counted per source (kind, instrument)
with a few sample rows, and the dataset's policy decides what happens to the affected rows:

- ``reject``: the whole ingest fails (nothing is written);
- ``drop``:   the rows are removed and counted;
- ``flag``:   the rows are kept and counted.

Some codes leave no usable value (a missing or unparseable number, an off-grid price), so they can
only be rejected or dropped. ``TS_UNIT_AMBIGUOUS`` always rejects (TIME-6).
"""

from dataclasses import dataclass, field
from typing import Any

import numpy as np

REJECT, DROP, FLAG = "reject", "drop", "flag"
ACTIONS = (REJECT, DROP, FLAG)

# code -> (description, allowed actions)
CODES: dict[str, tuple[str, tuple[str, ...]]] = {
    "TS_UNIT_AMBIGUOUS": (
        "file timestamps are not all in one allowed unit's plausible range (TIME-6)",
        (REJECT,),
    ),
    "MALFORMED_ROW": ("row has the wrong number of columns", (REJECT, DROP)),
    "MISSING_VALUE": ("a required field is empty", (REJECT, DROP)),
    "BAD_NUMBER": (
        "a field is not a plain decimal number, or does not fit in int64",
        (REJECT, DROP),
    ),
    "OFF_TICK": ("price is not a whole number of ticks", (REJECT, DROP)),
    "OFF_LOT": ("quantity is not a whole number of lots", (REJECT, DROP)),
    "NON_POSITIVE_PRICE": ("price <= 0", ACTIONS),
    "NEGATIVE_QTY": ("quantity or volume < 0", ACTIONS),
    "OHLC_INCONSISTENT": ("bar violates low <= open, close <= high", ACTIONS),
    "CROSSED_QUOTE": ("bid > ask", ACTIONS),
    "BAD_CLOSE_TIME": ("source bar close time does not match open time + interval", ACTIONS),
    "BAD_VALUE": ("unrecognised categorical value (kept as unknown when flagged)", ACTIONS),
    "NON_MONOTONIC_TS": ("timestamp earlier than a previous row", (REJECT, DROP)),
    "DUPLICATE": ("repeats the previous row (bars: same open time)", ACTIONS),
    "MISALIGNED_BAR": ("bar open time is not a multiple of the interval", ACTIONS),
    "GAP": ("time between rows exceeds the expected interval", (REJECT, FLAG)),
}

_ALL_REJECT = {code: REJECT for code in CODES}

DEFAULT_POLICIES: dict[str, dict[str, str]] = {
    "bars": {**_ALL_REJECT, "GAP": FLAG},
    "quotes": {**_ALL_REJECT, "CROSSED_QUOTE": DROP, "DUPLICATE": FLAG},
    "trades": {**_ALL_REJECT, "DUPLICATE": FLAG},
}

MAX_SAMPLES = 5


def resolve_policy(kind: str, overrides: dict[str, str] | None) -> dict[str, str]:
    policy = dict(DEFAULT_POLICIES[kind])
    for code, action in (overrides or {}).items():
        if code not in CODES:
            raise ValueError(f"unknown validation code {code!r}")
        if action not in CODES[code][1]:
            raise ValueError(f"{code} allows {CODES[code][1]}, not {action!r}")
        policy[code] = action
    return policy


@dataclass
class Finding:
    code: str
    kind: str
    symbol: str
    action: str
    count: int = 0
    samples: list[dict[str, Any]] = field(default_factory=list)
    extra: dict[str, int] = field(default_factory=dict)

    def to_dict(self, with_samples: bool = True) -> dict[str, Any]:
        d: dict[str, Any] = {
            "code": self.code,
            "kind": self.kind,
            "symbol": self.symbol,
            "action": self.action,
            "count": self.count,
        }
        d.update(self.extra)
        if with_samples:
            d["samples"] = self.samples
        return d


@dataclass
class SourceStats:
    kind: str
    symbol: str
    rows_read: int = 0
    rows_written: int = 0

    @property
    def rows_dropped(self) -> int:
        return self.rows_read - self.rows_written


class Report:
    """Accumulates findings across batches and files."""

    def __init__(self) -> None:
        self._findings: dict[tuple[str, str, str], Finding] = {}
        self.sources: list[SourceStats] = []

    def add(
        self,
        code: str,
        kind: str,
        symbol: str,
        action: str,
        rows: np.ndarray | list[int],
        file: str,
        **extra: int,
    ) -> None:
        """Count `rows` (1-based data row numbers in `file`) under `code`."""
        n = len(rows)
        if n == 0:
            return
        key = (kind, symbol, code)
        f = self._findings.get(key)
        if f is None:
            f = self._findings[key] = Finding(code=code, kind=kind, symbol=symbol, action=action)
        f.count += n
        for k, v in extra.items():
            f.extra[k] = f.extra.get(k, 0) + int(v)
        for r in list(rows[: MAX_SAMPLES - len(f.samples)]):
            f.samples.append({"file": file, "row": int(r)})

    @property
    def findings(self) -> list[Finding]:
        order = {c: i for i, c in enumerate(CODES)}
        return sorted(self._findings.values(), key=lambda f: (f.kind, f.symbol, order[f.code]))

    def counts(self) -> dict[str, int]:
        """code -> total count (all sources), for tests and quick summaries."""
        out: dict[str, int] = {}
        for f in self._findings.values():
            out[f.code] = out.get(f.code, 0) + f.count
        return out

    @property
    def rejected(self) -> bool:
        return any(f.action == REJECT for f in self._findings.values())

    @property
    def status(self) -> str:
        if self.rejected:
            return "rejected"
        return "flagged" if self._findings else "clean"

    def to_dict(self, with_samples: bool = True) -> dict[str, Any]:
        return {
            "status": self.status,
            "rows_read": sum(s.rows_read for s in self.sources),
            "rows_written": sum(s.rows_written for s in self.sources),
            "rows_dropped": sum(s.rows_dropped for s in self.sources),
            "findings": [f.to_dict(with_samples) for f in self.findings],
        }


class DatasetRejected(Exception):
    """Raised by ingest when any finding's action is reject. Carries the full report."""

    def __init__(self, report: Report):
        self.report = report
        codes = sorted({f.code for f in report.findings if f.action == REJECT})
        super().__init__(f"dataset rejected: {', '.join(codes)}")
