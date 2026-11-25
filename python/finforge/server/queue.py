"""Job queue on Postgres (PLAN Phase 7 task 3, D7).

- claim: one queued job per call, `FOR UPDATE SKIP LOCKED`, so concurrent workers never take the
  same job and never block each other; the claim sets a lease (`lease_until`).
- heartbeat: renews the lease and records progress, only while the job is still this worker's;
  returns `cancel_requested`, or None when the lease was lost.
- reap: jobs whose lease expired (their worker died) are queued again while attempts remain, else
  failed; their runs follow.
- finish: final status, again only if the job is still this worker's.

All times come from the database clock (now()), so workers need not agree on theirs.
"""

import json
from dataclasses import dataclass
from typing import Any

from sqlalchemy import text
from sqlalchemy.engine import Connection, Engine


@dataclass
class Claimed:
    job_id: int
    run_id: str
    attempt: int
    config: dict[str, Any]
    dataset_id: str


def claim(engine: Engine, worker_id: str, lease_seconds: float) -> Claimed | None:
    with engine.begin() as c:
        row = c.execute(
            text("""
            UPDATE jobs SET status = 'running', attempts = attempts + 1, worker_id = :w,
                   lease_until = now() + make_interval(secs => :lease), updated_at = now(),
                   progress = 0, events_processed = 0, sim_time_ns = NULL
            WHERE id = (SELECT id FROM jobs WHERE status = 'queued'
                        ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1)
            RETURNING id, run_id, attempts
        """),
            {"w": worker_id, "lease": lease_seconds},
        ).first()
        if row is None:
            return None
        run = c.execute(
            text("""
            UPDATE runs SET status = 'running', started_at = now(), error = NULL
            WHERE id = :r RETURNING config, dataset_id
        """),
            {"r": row.run_id},
        ).one()
        return Claimed(row.id, row.run_id, row.attempts, run.config, run.dataset_id)


def heartbeat(
    engine: Engine,
    job_id: int,
    worker_id: str,
    lease_seconds: float,
    progress: float,
    events: int,
    sim_time_ns: int | None,
) -> bool | None:
    with engine.begin() as c:
        row = c.execute(
            text("""
            UPDATE jobs SET lease_until = now() + make_interval(secs => :lease), progress = :p,
                   events_processed = :e, sim_time_ns = :t, updated_at = now()
            WHERE id = :id AND worker_id = :w AND status = 'running'
            RETURNING cancel_requested
        """),
            {
                "id": job_id,
                "w": worker_id,
                "lease": lease_seconds,
                "p": progress,
                "e": events,
                "t": sim_time_ns,
            },
        ).first()
        return None if row is None else bool(row.cancel_requested)


def finish(c: Connection, job_id: int, worker_id: str, job_status: str, error: str | None) -> bool:
    """Inside the caller's transaction. False if the job is no longer this worker's."""
    row = c.execute(
        text("""
        UPDATE jobs SET status = CAST(:s AS varchar), lease_until = NULL,
               last_error = CAST(:err AS text), updated_at = now(),
               progress = CASE WHEN CAST(:s AS varchar) = 'done' THEN 1.0 ELSE progress END
        WHERE id = :id AND worker_id = :w AND status = 'running'
        RETURNING id
    """),
        {"id": job_id, "w": worker_id, "s": job_status, "err": error},
    ).first()
    return row is not None


def reap(engine: Engine) -> list[tuple[int, str, str]]:
    """Expired leases -> retry or fail. Returns (job id, run id, new job status)."""
    with engine.begin() as c:
        rows = c.execute(
            text("""
            UPDATE jobs
               SET status = CASE WHEN attempts < max_attempts THEN 'queued' ELSE 'failed' END,
                   worker_id = NULL, lease_until = NULL, updated_at = now(),
                   last_error = 'lease expired: worker lost during attempt ' || attempts
            WHERE status = 'running' AND lease_until < now()
            RETURNING id, run_id, status, attempts
        """)
        ).all()
        for r in rows:
            if r.status == "queued":
                c.execute(text("UPDATE runs SET status = 'queued' WHERE id = :r"), {"r": r.run_id})
            else:
                c.execute(
                    text("""
                    UPDATE runs SET status = 'failed', finished_at = now(), partial = true,
                           error = CAST(:e AS jsonb)
                    WHERE id = :r
                """),
                    {
                        "r": r.run_id,
                        "e": json.dumps(
                            {
                                "type": "WorkerLost",
                                "message": f"the worker stopped heartbeating "
                                f"(attempt {r.attempts})",
                            }
                        ),
                    },
                )
        return [(r.id, r.run_id, r.status) for r in rows]


def request_cancel(engine: Engine, run_id: str) -> str | None:
    """'cancelled' if the job was still queued (cancelled now), 'requested' if running (the worker
    stops it), None if already finished."""
    with engine.begin() as c:
        row = c.execute(
            text("""
            UPDATE jobs SET status = 'cancelled', updated_at = now()
            WHERE run_id = :r AND status = 'queued' RETURNING id
        """),
            {"r": run_id},
        ).first()
        if row is not None:
            c.execute(
                text("UPDATE runs SET status = 'cancelled', finished_at = now() WHERE id = :r"),
                {"r": run_id},
            )
            return "cancelled"
        row = c.execute(
            text("""
            UPDATE jobs SET cancel_requested = true, updated_at = now()
            WHERE run_id = :r AND status = 'running' RETURNING id
        """),
            {"r": run_id},
        ).first()
        return "requested" if row is not None else None
