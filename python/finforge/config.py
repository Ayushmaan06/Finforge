"""Backtest configuration (PLAN Phase 5 task 3): validated with pydantic, loaded from YAML.

Every money amount, rate and fraction is a decimal (YAML floats are read through their shortest
text, so `0.1` is exactly 0.1) and is converted to the engine's integer units exactly; a value that
does not convert exactly is a validation error, never a silent rounding.

    dataset: datasets/normalized/binance-spot-1m
    universe: [BTCUSDT]
    start: 2024-01-01T00:00:00Z
    end: 2024-02-01T00:00:00Z
    initial_capital: 10000            # quote currency
    execution: {model: candle, slippage: {kind: fixed_bps, bps: 2}, latency_ns: 1}
    fees: {maker_bps: 2, taker_bps: 10}
    risk: {allow_short: false, max_leverage: 1}
    seed: 42
    strategy: {name: ma_cross, params: {fast: 20, slow: 100, qty: "0.01"}}
"""

from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any, Literal

import yaml
from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

MONEY_SCALE = 100_000_000  # contracts §1: Money = 1e-8 of the quote currency
RATE_SCALE = 1_000_000  # Rate = 1e-6
BPS_TO_RATE = 100  # 1 bp = 1e-4 = 100 Rate units


def _exact_int(value: Decimal, scale: int, what: str) -> int:
    scaled = value * scale
    if scaled != scaled.to_integral_value():
        raise ValueError(f"{what} {value} is finer than the engine's resolution (1/{scale})")
    return int(scaled)


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)


class Slippage(_Model):
    kind: Literal["none", "fixed_bps", "half_spread"] = "none"
    bps: Decimal = Field(default=Decimal(0), ge=0, lt=10_000)  # half_spread: the full spread

    @model_validator(mode="after")
    def _kind_needs_bps(self) -> "Slippage":
        if self.kind == "none" and self.bps != 0:
            raise ValueError("slippage bps given with kind 'none'")
        return self

    @property
    def rate(self) -> int:
        return _exact_int(self.bps, BPS_TO_RATE, "slippage bps")


class Execution(_Model):
    model: Literal["candle", "quote"] = "candle"
    slippage: Slippage = Slippage()
    participation: Decimal = Field(default=Decimal(0), ge=0, le=1)  # candle volume cap; 0 = off
    latency_ns: int = Field(default=1, ge=1)  # EVT-3

    @model_validator(mode="after")
    def _model_options(self) -> "Execution":
        if self.model == "quote" and (self.slippage.kind != "none" or self.participation != 0):
            raise ValueError(
                "the quote model fills at the quote: no slippage or participation (SLP-2)"
            )
        return self


class Fees(_Model):
    maker_bps: Decimal = Field(default=Decimal(0), ge=0)
    taker_bps: Decimal = Field(default=Decimal(0), ge=0)
    min_fee: Decimal = Field(default=Decimal(0), ge=0)  # quote currency, per fill


class Risk(_Model):
    allow_short: bool = False
    max_position_lots: int | None = Field(default=None, ge=1)  # per instrument; None = no limit
    max_leverage: Decimal = Field(default=Decimal(1), gt=0)
    borrow_rate: Decimal = Field(default=Decimal(0), ge=0)  # annual fraction, on shorts (ACC-8)


class StrategyRef(_Model):
    # A built-in name (finforge.strategies), "package.module:Class" or "path/to/file.py:Class".
    name: str
    params: dict[str, Any] = {}


class BacktestConfig(_Model):
    dataset: str  # dataset directory (finforge.data)
    dataset_id: str | None = None  # if given, must match the dataset's manifest id
    universe: list[str] = Field(min_length=1)
    start: datetime | None = None
    end: datetime | None = None
    initial_capital: Decimal = Field(gt=0)
    execution: Execution = Execution()
    fees: Fees = Fees()
    risk: Risk = Risk()
    seed: int = 0
    strategy: StrategyRef
    equity_interval_s: int | None = Field(default=None, ge=1)  # None: one point per bar
    output_dir: str = "runs"
    benchmark: bool = True  # also run buy-and-hold of the universe (PLAN §5.9) for the report

    @field_validator("start", "end")
    @classmethod
    def _utc(cls, v: datetime | None) -> datetime | None:
        if v is not None and (v.tzinfo is None or v.utcoffset() is None):
            raise ValueError("times must carry a timezone (e.g. 2024-01-01T00:00:00Z); TIME-1")
        return v

    @field_validator("universe")
    @classmethod
    def _unique(cls, v: list[str]) -> list[str]:
        if len(set(v)) != len(v):
            raise ValueError("universe has duplicate symbols")
        return v

    @model_validator(mode="after")
    def _checks(self) -> "BacktestConfig":
        if self.start and self.end and self.end <= self.start:
            raise ValueError("end must be after start")
        # Fail early on values the engine cannot represent exactly.
        self.engine_account()
        _ = self.execution.slippage.rate
        _exact_int(self.execution.participation, RATE_SCALE, "participation")
        return self

    # --- engine units ---

    @staticmethod
    def _ns(t: datetime | None) -> int | None:
        """Exact ns since the Unix epoch (timedelta arithmetic is integer, never float)."""
        if t is None:
            return None
        d = t - datetime(1970, 1, 1, tzinfo=UTC)
        return (d.days * 86_400 + d.seconds) * 1_000_000_000 + d.microseconds * 1_000

    @property
    def start_ns(self) -> int | None:
        return self._ns(self.start)

    @property
    def end_ns(self) -> int | None:
        return self._ns(self.end)

    def engine_account(self) -> dict[str, Any]:
        """Keyword arguments for `_engine.Backtest.open_account`."""
        r = self.risk
        return {
            "cash": _exact_int(self.initial_capital, MONEY_SCALE, "initial_capital"),
            "maker": _exact_int(self.fees.maker_bps, BPS_TO_RATE, "maker_bps"),
            "taker": _exact_int(self.fees.taker_bps, BPS_TO_RATE, "taker_bps"),
            "min_fee": _exact_int(self.fees.min_fee, MONEY_SCALE, "min_fee"),
            "allow_short": r.allow_short,
            "max_position": r.max_position_lots or 0,
            "max_leverage": _exact_int(r.max_leverage, RATE_SCALE, "max_leverage"),
            "borrow_rate": _exact_int(r.borrow_rate, RATE_SCALE, "borrow_rate"),
        }

    @classmethod
    def from_yaml(cls, path: str | Path) -> "BacktestConfig":
        data = yaml.safe_load(Path(path).read_text())
        if not isinstance(data, dict):
            raise ValueError(f"{path}: expected a mapping at the top level")
        return cls.model_validate(data)

    def to_json_dict(self) -> dict[str, Any]:
        return self.model_dump(mode="json")
