"""Historical data pipeline: ingest → validate → normalised dataset → replay (PLAN Phase 3).

See docs/data.md for schemas, formats, validation codes and the manifest.
"""

from finforge.data.dataset import Dataset, ReplayResult, replay
from finforge.data.fixedpoint import Grid, parse_on_grid, parse_scaled
from finforge.data.ingest import DatasetSpec, IngestResult, ingest
from finforge.data.registry import Instrument, Registry
from finforge.data.schema import KINDS, SCHEMA_VERSION, SCHEMAS
from finforge.data.validate import CODES, DEFAULT_POLICIES, DatasetRejected, Report

__all__ = [
    "CODES",
    "DEFAULT_POLICIES",
    "KINDS",
    "SCHEMAS",
    "SCHEMA_VERSION",
    "Dataset",
    "DatasetRejected",
    "DatasetSpec",
    "Grid",
    "IngestResult",
    "Instrument",
    "Registry",
    "ReplayResult",
    "Report",
    "ingest",
    "parse_on_grid",
    "parse_scaled",
    "replay",
]
