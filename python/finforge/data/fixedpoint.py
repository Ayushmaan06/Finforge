"""Exact decimal text → fixed-point integers (contracts.md NUM-1, INS-1).

Nothing here goes through a float. Source values are parsed as text; the digits after the decimal
point are padded to a fixed scale and the whole digit string is cast to int64. A value with more
significant fractional digits than the scale allows is *off-grid*, which the caller reports as
OFF_TICK / OFF_LOT.
"""

import re
from dataclasses import dataclass
from decimal import Decimal

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc

_NUMBER = r"^(?P<neg>-?)(?P<int>[0-9]+)(?:\.(?P<frac>[0-9]*))?$"
_POSITIVE_DECIMAL = re.compile(r"^[0-9]+(\.[0-9]+)?$")
_INT64_MAX = str(2**63 - 1)  # 19 digits


@dataclass(frozen=True)
class Grid:
    """A tick or lot size: value = units * 10^-digits, e.g. "0.50" → Grid(units=5, digits=1)."""

    text: str
    units: int
    digits: int

    @classmethod
    def parse(cls, text: str) -> "Grid":
        if not isinstance(text, str) or not _POSITIVE_DECIMAL.match(text):
            raise ValueError(f"grid size must be a positive decimal string, got {text!r}")
        whole, _, frac = text.partition(".")
        frac = frac.rstrip("0")
        units = int(whole + frac)
        if units <= 0:
            raise ValueError(f"grid size must be > 0, got {text!r}")
        return cls(text=text, units=units, digits=len(frac))


@dataclass
class Parsed:
    """Result of parsing a column. `values` is 0 wherever a mask is set."""

    values: np.ndarray  # int64, in units of 10^-digits
    missing: np.ndarray  # bool: null or empty
    bad: np.ndarray  # bool: not a plain decimal number, or too many digits for int64
    off_grid: np.ndarray  # bool: significant digits beyond the scale

    @property
    def ok(self) -> np.ndarray:
        return ~(self.missing | self.bad | self.off_grid)


def _np_bool(arr: pa.Array) -> np.ndarray:
    return np.asarray(arr.fill_null(False).to_numpy(zero_copy_only=False), dtype=bool)


def _expand_exponents(column: pa.Array) -> pa.Array:
    """Arrow casts some doubles to text in scientific notation ("1e-07", "1e+16"); rewrite those
    rows as plain decimals so the exact parser accepts them. Only for float sources: in a text
    source an exponent stays BAD_NUMBER (contracts INS-1)."""
    mask = pc.fill_null(pc.match_substring(column, "e"), False)
    if not pc.any(mask).as_py():
        return column
    sci = pc.filter(column, mask).to_pylist()  # rare rows: a Python loop is fine
    plain = pa.array([format(Decimal(s), "f") for s in sci], pa.string())
    return pc.replace_with_mask(column, mask, plain)


def parse_scaled(column: pa.Array | pa.ChunkedArray, digits: int) -> Parsed:
    """Parse decimal text to integers in units of 10^-digits, exactly."""
    if isinstance(column, pa.ChunkedArray):
        column = column.combine_chunks()
    if not pa.types.is_string(column.type) and not pa.types.is_large_string(column.type):
        floats = pa.types.is_floating(column.type)
        column = pc.cast(column, pa.string())
        if floats:
            column = _expand_exponents(column)
    s = pc.utf8_trim_whitespace(column)
    missing = _np_bool(pc.or_kleene(pc.is_null(s), pc.equal(s, "")))

    m = pc.extract_regex(s, _NUMBER)
    matched = _np_bool(pc.is_valid(m))
    neg = pc.struct_field(m, "neg")
    int_part = pc.utf8_ltrim(pc.struct_field(m, "int"), characters="0")
    frac = pc.utf8_rtrim(pc.struct_field(m, "frac"), characters="0")

    off_grid = _np_bool(pc.greater(pc.utf8_length(frac), digits)) & matched
    padded = pc.utf8_rpad(frac, width=digits, padding="0") if digits > 0 else frac
    text = pc.binary_join_element_wise(int_part, padded, "")
    # Does the digit string fit in int64? Same-length digit strings compare like numbers.
    n_digits = pc.utf8_length(text)
    too_long = (
        matched
        & ~off_grid
        & _np_bool(
            pc.or_(
                pc.greater(n_digits, len(_INT64_MAX)),
                pc.and_(pc.equal(n_digits, len(_INT64_MAX)), pc.greater(text, _INT64_MAX)),
            )
        )
    )
    bad = ~missing & (~matched | too_long)
    valid = ~(missing | bad | off_grid)

    # Rows that are not valid, and the empty string (from "0" / "0.000"), become "0".
    text = pc.if_else(pa.array(valid), text, "0")
    text = pc.if_else(pc.equal(text, ""), "0", text)
    values = np.asarray(pc.cast(text, pa.int64()).to_numpy(zero_copy_only=False), dtype=np.int64)
    values = np.where(_np_bool(pc.equal(neg, "-")) & valid, -values, values)
    return Parsed(values=values, missing=missing, bad=bad, off_grid=off_grid)


def parse_on_grid(column: pa.Array | pa.ChunkedArray, grid: Grid) -> Parsed:
    """Parse decimal text to a whole number of grid steps (ticks or lots)."""
    p = parse_scaled(column, grid.digits)
    rem = np.remainder(p.values, grid.units)
    off = p.off_grid | ((rem != 0) & ~(p.missing | p.bad))
    values = np.where(off, 0, p.values // grid.units)
    return Parsed(values=values, missing=p.missing, bad=p.bad, off_grid=off)


def parse_int(column: pa.Array | pa.ChunkedArray) -> Parsed:
    """Integers (e.g. source timestamps). A fractional part makes the value bad."""
    p = parse_scaled(column, 0)
    return Parsed(
        values=p.values, missing=p.missing, bad=p.bad | p.off_grid, off_grid=p.off_grid & False
    )
