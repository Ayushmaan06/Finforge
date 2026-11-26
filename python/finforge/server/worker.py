"""Background worker (PLAN Phase 7 task 3).

Loop: reap expired leases -> claim one queued job -> run its backtest in a child process -> record
the result. While the child runs, the worker heartbeats every `heartbeat_seconds`: it renews the
lease, stores progress, and passes `cancel_requested` down to the child, which raises the engine's
cancel flag (the run stops between two events and ends `cancelled`).

The child talks to the parent over a pipe and never touches the database. If the worker dies, its
lease expires and another worker's reaper retries or fails the job; the orphaned child notices its
parent is gone at its next progress report and stops.
"""

import contextlib
import logging
import multiprocessing as mp
import os
import signal
import socket
import threading
import time
import traceback
import uuid
from multiprocessing.connection import Connection
from typing import Any

from sqlalchemy.orm import Session

from finforge.server import queue, service
from finforge.server.db import Dataset, make_engine
from finforge.server.settings import Settings

log = logging.getLogger("finforge.worker")


def _child(
    config: dict[str, Any], run_dir: str, conn: Connection, parent_pid: int, progress_every: int
) -> None:
    """Runs in a fresh (spawned) process: one engine per process (PLAN §4.3)."""
    from finforge.backtest import run

    signal.signal(signal.SIGINT, signal.SIG_IGN)  # the parent decides when to stop
    cancel = False

    def progress(events: int, now: int) -> bool:
        nonlocal cancel
        if os.getppid() != parent_pid:  # the worker died: stop instead of running unsupervised
            return True
        conn.send(("progress", events, now))
        while conn.poll():
            if conn.recv()[0] == "cancel":
                cancel = True
        return cancel

    try:
        result = run(config, run_dir=run_dir, progress=progress, progress_every=progress_every)
        conn.send(("done", result.status))
    except BaseException:  # configuration/data errors raised before the run could start
        conn.send(("error", traceback.format_exc()))
        raise


class Worker:
    def __init__(self, settings: Settings, worker_id: str | None = None):
        self.settings = settings
        self.engine = make_engine(settings.database_url)
        self.worker_id = worker_id or f"{socket.gethostname()}:{os.getpid()}:{uuid.uuid4().hex[:6]}"
        self._ctx = mp.get_context("spawn")

    def run_forever(self, stop: threading.Event | None = None) -> None:
        stop = stop or threading.Event()
        log.info("worker %s started", self.worker_id)
        while not stop.is_set():
            try:
                busy = self.run_once()
            except Exception:  # e.g. the database is briefly unavailable: keep the worker alive
                log.exception("worker loop error")
                busy = False
            if not busy:
                stop.wait(self.settings.poll_seconds)

    def run_once(self) -> bool:
        """Reap, then run at most one job. True if a job was claimed."""
        for job_id, run_id, status in queue.reap(self.engine):
            log.warning("job %s (run %s) lost its worker: now %s", job_id, run_id, status)
        job = queue.claim(self.engine, self.worker_id, self.settings.lease_seconds)
        if job is None:
            return False
        self._execute(job)
        return True

    def _execute(self, job: queue.Claimed) -> None:
        s = self.settings
        with Session(self.engine) as session:
            dataset = session.get(Dataset, job.dataset_id)
        run_dir = s.runs_dir / job.run_id / f"attempt-{job.attempt}"
        config = job.config
        parent, child = self._ctx.Pipe()
        proc = self._ctx.Process(
            target=_child,
            args=(config, str(run_dir), child, os.getpid(), s.progress_every),
            daemon=True,
        )
        proc.start()
        child.close()
        events, sim_now, child_error, done = 0, None, None, False
        next_beat = time.monotonic()
        lost = False
        while True:
            try:
                ready = parent.poll(min(0.2, s.heartbeat_seconds))
                msg = parent.recv() if ready else ()
            except (EOFError, OSError):  # the child exited and closed its end of the pipe
                break
            if msg:
                if msg[0] == "progress":
                    events, sim_now = msg[1], msg[2]
                elif msg[0] == "done":
                    done = True
                elif msg[0] == "error":
                    child_error = msg[1]
            if not proc.is_alive():
                try:
                    if not parent.poll():
                        break
                except (EOFError, OSError):
                    break
            if time.monotonic() >= next_beat:
                next_beat = time.monotonic() + s.heartbeat_seconds
                frac = (
                    service.progress_fraction(config, dataset, sim_now)
                    if dataset is not None and sim_now is not None
                    else 0.0
                )
                try:
                    cancel = queue.heartbeat(
                        self.engine,
                        job.job_id,
                        self.worker_id,
                        s.lease_seconds,
                        frac,
                        events,
                        sim_now,
                    )
                except Exception:  # a database blip: the lease outlives several missed beats
                    log.warning("job %s: heartbeat failed; retrying", job.job_id, exc_info=True)
                    continue
                if cancel is None:  # the lease is gone: someone else owns the job now
                    lost = True
                    proc.kill()
                    break
                if cancel:
                    with contextlib.suppress(OSError):  # the child may just have exited
                        parent.send(("cancel",))
        proc.join(timeout=30)
        if proc.is_alive():
            proc.kill()
            proc.join()
        if lost:
            log.warning("job %s: lease lost; result discarded", job.job_id)
            return
        if not done and child_error is None and proc.exitcode != 0:
            child_error = f"backtest process exited with code {proc.exitcode}"
        status = self._finalize(job, run_dir, child_error)
        log.info("job %s (run %s): %s", job.job_id, job.run_id, status)

    def _finalize(self, job: queue.Claimed, run_dir: Any, child_error: str | None) -> str | None:
        """Record the result, retrying a failing database so a blip does not throw away a finished
        run. The waits total 7/16 of the lease, so with the last heartbeat at most one beat ago
        they end before the lease expires. finalize is one transaction guarded by the lease, so
        a retry can never write twice."""
        delay = self.settings.lease_seconds / 16
        for attempt in range(4):  # waits: 1/16, 1/8, 1/4 of the lease
            try:
                return service.finalize(
                    self.engine, job.job_id, self.worker_id, job.run_id, run_dir, child_error
                )
            except Exception:
                if attempt == 3:
                    raise  # the lease will expire and the reaper retries or fails the job
                log.warning(
                    "job %s: recording the result failed; retrying", job.job_id, exc_info=True
                )
                time.sleep(delay)
                delay *= 2
        return None
