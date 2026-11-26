"""Response models. They make the OpenAPI document precise, which the dashboard's typed client is
generated from (frontend/src/lib/api-schema.d.ts)."""

from typing import Any, Literal

from pydantic import BaseModel


class Health(BaseModel):
    status: str
    engine: str


class UploadOut(BaseModel):
    id: str
    filename: str
    size: int
    sha256: str
    content_type: str
    created_at: str


class InstrumentOut(BaseModel):
    instrument_id: int
    symbol: str
    asset_class: str
    quote_currency: str
    tick_size: str
    lot_size: str
    min_qty: int
    contract_multiplier: int


class DatasetFile(BaseModel):
    kind: str
    symbol: str
    rows: int


class DatasetBrief(BaseModel):
    id: str
    name: str
    schema_version: int
    time_first_ns: int | None
    time_last_emit_ns: int | None
    validation_status: str
    files: list[DatasetFile]
    created_at: str


class DatasetOut(DatasetBrief):
    instruments: list[InstrumentOut]
    validation: dict[str, Any]


class StrategyBrief(BaseModel):
    name: str
    summary: str


class StrategyOut(BaseModel):
    name: str
    description: str
    params_schema: dict[str, Any]
    defaults: dict[str, Any]
    source_sha256: str


class RunSubmitted(BaseModel):
    id: str
    status: str


class JobOut(BaseModel):
    status: str
    attempts: int
    max_attempts: int
    progress: float
    events_processed: int
    cancel_requested: bool
    last_error: str | None


class MetricsOut(BaseModel):
    total_return: float | None
    cagr: float | None
    volatility: float | None
    sharpe: float | None
    sortino: float | None
    max_drawdown: float | None
    var: float | None
    cvar: float | None
    trades: int | None
    win_rate: float | None
    profit_factor: float | None
    fees: float | None
    benchmark_return: float | None


class RunOut(BaseModel):
    id: str
    status: Literal["queued", "running", "completed", "failed", "cancelled"]
    dataset_id: str
    strategy: str
    params: dict[str, Any]
    config: dict[str, Any]
    created_at: str
    started_at: str | None
    finished_at: str | None
    wall_seconds: float | None
    journal_hash: str | None
    strategy_hash: str | None
    engine_version: str | None
    reconciles: bool | None
    partial: bool | None
    error: dict[str, Any] | None
    job: JobOut
    metrics: MetricsOut | None


class CancelOut(BaseModel):
    id: str
    cancel: Literal["cancelled", "requested"]


class EquityOut(BaseModel):
    """Downsampled equity (min/max per bucket); the drawdown is computed on the full curve first,
    from the initial capital, and the trough and peak of the maximum drawdown are always kept."""

    ts: list[int]
    equity: list[float]
    drawdown: list[float]
    benchmark: list[float | None] | None
    points_total: int


class ReturnsOut(BaseModel):
    day_ts: list[int]
    returns: list[float]
    benchmark: list[float] | None


class Marker(BaseModel):
    ts: int
    side: Literal["buy", "sell"]
    price: float
    qty: float


class PricesOut(BaseModel):
    symbol: str
    kind: Literal["bars", "quotes"]
    ts: list[int]
    price: list[float]
    markers: list[Marker]
    points_total: int
    markers_total: int


class TradesOut(BaseModel):
    total: int
    offset: int
    trades: list[dict[str, Any]]


class CompareOut(BaseModel):
    rows: list[dict[str, Any]]
    series: dict[str, list[Any]]
