"""Initial schema: uploads, datasets, instruments, strategies, runs, run_metrics, jobs.

Revision ID: 0001
Revises:
Create Date: 2026-11-25
"""

import sqlalchemy as sa
from alembic import op
from sqlalchemy.dialects.postgresql import JSONB

revision = "0001"
down_revision = None
branch_labels = None
depends_on = None

TS = sa.DateTime(timezone=True)


def upgrade() -> None:
    op.create_table(
        "uploads",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("original_name", sa.String(120), nullable=False),
        sa.Column("stored_path", sa.Text, nullable=False),
        sa.Column("size", sa.BigInteger, nullable=False),
        sa.Column("sha256", sa.String(64), nullable=False),
        sa.Column("content_type", sa.String(16), nullable=False),
        sa.Column("created_at", TS, server_default=sa.func.now(), nullable=False),
    )
    op.create_table(
        "datasets",
        sa.Column("id", sa.String(64), primary_key=True),
        sa.Column("name", sa.String(120), nullable=False),
        sa.Column("path", sa.Text, nullable=False),
        sa.Column("schema_version", sa.Integer, nullable=False),
        sa.Column("time_first_ns", sa.BigInteger),
        sa.Column("time_last_emit_ns", sa.BigInteger),
        sa.Column("files", JSONB, nullable=False),
        sa.Column("validation", JSONB, nullable=False),
        sa.Column("manifest", JSONB, nullable=False),
        sa.Column("created_at", TS, server_default=sa.func.now(), nullable=False),
    )
    op.create_table(
        "instruments",
        sa.Column(
            "dataset_id",
            sa.String(64),
            sa.ForeignKey("datasets.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        sa.Column("instrument_id", sa.Integer, primary_key=True),
        sa.Column("symbol", sa.String(64), nullable=False),
        sa.Column("asset_class", sa.String(32), nullable=False),
        sa.Column("quote_currency", sa.String(16), nullable=False),
        sa.Column("tick_size", sa.String(32), nullable=False),
        sa.Column("lot_size", sa.String(32), nullable=False),
        sa.Column("min_qty", sa.BigInteger, nullable=False),
        sa.Column("contract_multiplier", sa.BigInteger, nullable=False),
    )
    op.create_table(
        "strategies",
        sa.Column("name", sa.String(64), primary_key=True),
        sa.Column("description", sa.Text, nullable=False),
        sa.Column("params_schema", JSONB, nullable=False),
        sa.Column("defaults", JSONB, nullable=False),
        sa.Column("source_sha256", sa.String(64), nullable=False),
        sa.Column("updated_at", TS, server_default=sa.func.now(), nullable=False),
    )
    op.create_table(
        "runs",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("status", sa.String(16), nullable=False, index=True),
        sa.Column("dataset_id", sa.String(64), sa.ForeignKey("datasets.id"), nullable=False),
        sa.Column("strategy", sa.String(64), sa.ForeignKey("strategies.name"), nullable=False),
        sa.Column("config", JSONB, nullable=False),
        sa.Column("strategy_hash", sa.String(64)),
        sa.Column("journal_hash", sa.String(64)),
        sa.Column("engine_version", sa.String(128)),
        sa.Column("created_at", TS, server_default=sa.func.now(), nullable=False),
        sa.Column("started_at", TS),
        sa.Column("finished_at", TS),
        sa.Column("wall_seconds", sa.Float),
        sa.Column("error", JSONB),
        sa.Column("artefact_dir", sa.Text),
        sa.Column("partial", sa.Boolean),
        sa.Column("reconciles", sa.Boolean),
        sa.CheckConstraint(
            "status in ('queued','running','completed','failed','cancelled')", name="runs_status"
        ),
    )
    op.create_table(
        "run_metrics",
        sa.Column(
            "run_id", sa.String(36), sa.ForeignKey("runs.id", ondelete="CASCADE"), primary_key=True
        ),
        *[
            sa.Column(c, sa.Float)
            for c in (
                "total_return",
                "cagr",
                "volatility",
                "sharpe",
                "sortino",
                "max_drawdown",
                "var",
                "cvar",
            )
        ],
        sa.Column("trades", sa.Integer),
        sa.Column("win_rate", sa.Float),
        sa.Column("profit_factor", sa.Float),
        sa.Column("fees", sa.Float),
        sa.Column("benchmark_return", sa.Float),
    )
    op.create_table(
        "jobs",
        sa.Column("id", sa.BigInteger, primary_key=True, autoincrement=True),
        sa.Column(
            "run_id",
            sa.String(36),
            sa.ForeignKey("runs.id", ondelete="CASCADE"),
            nullable=False,
            unique=True,
        ),
        sa.Column("status", sa.String(16), nullable=False, index=True),
        sa.Column("attempts", sa.Integer, nullable=False, server_default="0"),
        sa.Column("max_attempts", sa.Integer, nullable=False),
        sa.Column("lease_until", TS),
        sa.Column("worker_id", sa.String(64)),
        sa.Column("progress", sa.Float, nullable=False, server_default="0"),
        sa.Column("events_processed", sa.BigInteger, nullable=False, server_default="0"),
        sa.Column("sim_time_ns", sa.BigInteger),
        sa.Column("cancel_requested", sa.Boolean, nullable=False, server_default=sa.false()),
        sa.Column("last_error", sa.Text),
        sa.Column("created_at", TS, server_default=sa.func.now(), nullable=False),
        sa.Column("updated_at", TS, server_default=sa.func.now(), nullable=False),
        sa.CheckConstraint(
            "status in ('queued','running','done','failed','cancelled')", name="jobs_status"
        ),
    )
    # The queue scans queued jobs in id order.
    op.create_index("jobs_queued", "jobs", ["id"], postgresql_where=sa.text("status = 'queued'"))


def downgrade() -> None:
    for t in ("jobs", "run_metrics", "runs", "strategies", "instruments", "datasets", "uploads"):
        op.drop_table(t)
