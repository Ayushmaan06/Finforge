"""FinForge: market simulation and research platform.

The C++ engine is exposed as ``finforge._engine``; this package is the Python layer around it.
"""

from finforge._engine import engine_version

__version__ = "1.0.0"

__all__ = ["__version__", "engine_version"]
