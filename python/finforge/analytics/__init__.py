"""Performance and risk analytics, run comparison and parameter sweeps (PLAN Phase 6)."""

from finforge.analytics.metrics import Conventions, performance
from finforge.analytics.report import build_report, write_report
from finforge.analytics.trades import fifo_trades, trade_summary

__all__ = [
    "Conventions",
    "build_report",
    "fifo_trades",
    "performance",
    "trade_summary",
    "write_report",
]
