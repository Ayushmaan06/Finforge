"""Reference strategies (PLAN Phase 5 task 6) and the name → class registry used by configs."""

import importlib
import importlib.util
import sys
from pathlib import Path

from finforge.sdk import Strategy
from finforge.strategies.ma_cross import MovingAverageCrossover
from finforge.strategies.mean_reversion import MeanReversion
from finforge.strategies.momentum import TimeSeriesMomentum
from finforge.strategies.pairs import PairsTrading

BUILTIN: dict[str, type[Strategy]] = {
    cls.name: cls
    for cls in (MovingAverageCrossover, MeanReversion, TimeSeriesMomentum, PairsTrading)
}


def resolve(name: str) -> type[Strategy]:
    """A built-in name, "package.module:Class", or "path/to/file.py:Class"."""
    if name in BUILTIN:
        return BUILTIN[name]
    if ":" not in name:
        raise ValueError(f"unknown strategy {name!r}; built-ins: {sorted(BUILTIN)}")
    target, _, attr = name.rpartition(":")
    if target.endswith(".py"):
        path = Path(target).resolve()
        spec = importlib.util.spec_from_file_location(f"finforge_user_{path.stem}", path)
        if spec is None or spec.loader is None:
            raise ValueError(f"cannot load {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
    else:
        module = importlib.import_module(target)
    cls = getattr(module, attr, None)
    if not (isinstance(cls, type) and issubclass(cls, Strategy)):
        raise ValueError(f"{name!r} is not a finforge.sdk.Strategy subclass")
    return cls


__all__ = [
    "BUILTIN",
    "MeanReversion",
    "MovingAverageCrossover",
    "PairsTrading",
    "TimeSeriesMomentum",
    "resolve",
]
