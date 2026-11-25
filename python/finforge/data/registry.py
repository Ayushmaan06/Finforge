"""Instrument registry (contracts.md §3, PLAN Phase 3 task 2).

A registry file is JSON: {"instruments": [{...}, ...]}. Ids are dense from 0 in list order (INS-1).
Each instrument is checked with the engine's own NUM-2 rule, so Python and C++ cannot disagree.
"""

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from finforge._engine import money_per_tick_lot
from finforge.data.fixedpoint import Grid

_FIELDS = (
    "symbol",
    "asset_class",
    "quote_currency",
    "tick_size",
    "lot_size",
    "min_qty",
    "contract_multiplier",
)


@dataclass(frozen=True)
class Instrument:
    instrument_id: int
    symbol: str
    quote_currency: str
    tick_size: str  # decimal string, never a float (INS-1)
    lot_size: str
    asset_class: str = "spot"
    min_qty: int = 1  # lots
    contract_multiplier: int = 1

    @property
    def tick(self) -> Grid:
        return Grid.parse(self.tick_size)

    @property
    def lot(self) -> Grid:
        return Grid.parse(self.lot_size)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class Registry:
    def __init__(self, instruments: list[Instrument]):
        self.instruments = list(instruments)
        self._by_symbol = {i.symbol: i for i in self.instruments}

    @classmethod
    def from_dicts(cls, items: list[dict[str, Any]]) -> "Registry":
        out: list[Instrument] = []
        seen: set[str] = set()
        for idx, item in enumerate(items):
            unknown = set(item) - set(_FIELDS) - {"instrument_id"}
            if unknown:
                raise ValueError(f"instrument {idx}: unknown fields {sorted(unknown)}")
            if item.get("instrument_id", idx) != idx:
                raise ValueError(f"instrument {idx}: ids must be dense from 0 in list order")
            for key in ("tick_size", "lot_size"):
                if not isinstance(item.get(key), str):
                    raise ValueError(f"instrument {idx}: {key} must be a decimal string (INS-1)")
            inst = Instrument(instrument_id=idx, **{k: v for k, v in item.items() if k in _FIELDS})
            if inst.symbol in seen:
                raise ValueError(f"duplicate symbol {inst.symbol!r}")
            seen.add(inst.symbol)
            inst.tick  # noqa: B018 - validates the decimal
            inst.lot  # noqa: B018
            money_per_tick_lot(inst.tick_size, inst.lot_size)  # NUM-2; raises ValueError
            if inst.min_qty < 1 or inst.contract_multiplier < 1:
                raise ValueError(f"{inst.symbol}: min_qty and contract_multiplier must be >= 1")
            out.append(inst)
        if not out:
            raise ValueError("a registry needs at least one instrument")
        return cls(out)

    @classmethod
    def load(cls, path: str | Path) -> "Registry":
        data = json.loads(Path(path).read_text())
        return cls.from_dicts(data["instruments"])

    def to_dicts(self) -> list[dict[str, Any]]:
        return [i.to_dict() for i in self.instruments]

    def by_symbol(self, symbol: str) -> Instrument:
        try:
            return self._by_symbol[symbol]
        except KeyError:
            raise KeyError(f"unknown instrument {symbol!r}") from None

    def __len__(self) -> int:
        return len(self.instruments)

    def __iter__(self):
        return iter(self.instruments)
