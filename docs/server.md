# Server: API, persistence and jobs

FinForge as a service (PLAN.md Phase 7): an HTTP API, background workers that run backtests from a
queue in Postgres, and a registry of datasets, strategies and runs. Bulk data (market data and run
artefacts) stays in Parquet on disk; Postgres holds metadata, statuses, metric summaries and the
queue (D7). The interactive OpenAPI documentation is at **`/docs`**.

## 1. Running it

```bash
uv pip install -e ".[server]"
docker compose up -d db          # PostgreSQL 16 on localhost:5432 (dev only)
export FINFORGE_DATABASE_URL=postgresql+psycopg://finforge:finforge@localhost:5432/finforge
export FINFORGE_STORAGE_DIR=var  # uploads, datasets, run directories
finforge server migrate          # Alembic migrations
finforge server api --port 8000  # http://localhost:8000/docs
finforge server worker           # run as many as you like, on any machine with the storage
```

| Variable | Default | Meaning |
|---|---|---|
| `FINFORGE_DATABASE_URL` | `postgresql+psycopg://finforge:finforge@localhost:5432/finforge` | SQLAlchemy URL |
| `FINFORGE_STORAGE_DIR` | `var` | root for server-generated storage paths |
| `FINFORGE_MAX_UPLOAD_BYTES` | 512 MiB | upload size cap |
| `FINFORGE_LEASE_SECONDS` | 30 | a job's lease; a worker silent for this long is presumed dead |
| `FINFORGE_HEARTBEAT_SECONDS` | 5 | how often a worker renews its lease and reports progress |
| `FINFORGE_MAX_ATTEMPTS` | 2 | how many times a job may be attempted before it fails |
| `FINFORGE_POLL_SECONDS` | 1 | idle worker poll interval |
| `FINFORGE_PROGRESS_EVERY` | 8192 | engine events between progress reports |

Without Docker, the `pgserver` pip package provides PostgreSQL binaries. The test suite uses it
when `FINFORGE_TEST_DATABASE_URL` is not set.

## 2. Workflow

```text
POST /uploads                 raw file (CSV, single-file zip, Parquet, JSON) → upload id
POST /datasets/validate       spec referencing upload ids → validation report (nothing stored)
POST /datasets                same spec → ingested, versioned dataset (id = content hash)
GET  /strategies[/{name}]     built-in strategies, each with a JSON Schema for its params
POST /runs                    dataset id + universe + capital + model + fees + risk + strategy → 202
GET  /runs/{id}/events        server-sent events: status / progress … end
GET  /runs/{id}               status, job progress, hashes, metrics
GET  /runs/{id}/results       report.json      GET /runs/{id}/equity    GET /runs/{id}/trades
POST /runs/{id}/cancel        queued → cancelled at once; running → stops between two events
GET  /compare?runs=a,b        metrics table + aligned equity and drawdown
```

Example run request:

```json
{"dataset_id": "256e5d6e…", "universe": ["BTCUSDT"], "initial_capital": "10000",
 "start": "2024-01-01T00:00:00Z", "end": "2024-02-01T00:00:00Z",
 "execution": {"model": "candle", "slippage": {"kind": "fixed_bps", "bps": 1}},
 "fees": {"taker_bps": 10}, "strategy": {"name": "ma_cross", "params": {"fast": 30, "slow": 240}}}
```

## 3. Data model

| Table | Holds |
|---|---|
| `uploads` | server-generated path, size, SHA-256, sniffed type, sanitised original name |
| `datasets`, `instruments` | the manifest (id, files, time range, validation summary) and the registry |
| `strategies` | the built-in registry: description, parameter JSON Schema, defaults, source SHA-256 |
| `runs` | config (JSON), status, dataset, strategy and journal hashes, engine version, timings, error, artefact directory, partial and reconciliation flags |
| `run_metrics` | headline metrics from `report.json`, for listing and comparing without reading files |
| `jobs` | one per run: status, attempts and maximum, `lease_until`, worker id, progress, `cancel_requested` |

Run statuses are `queued → running → completed | failed | cancelled`. A retried run goes back to
`queued`. Each attempt writes to its own directory, `runs/<run id>/attempt-<n>`.

## 4. The job queue

All times come from the database clock (`now()`).

- **Claim:**

  ```sql
  UPDATE jobs SET status='running', attempts=attempts+1, worker_id=…, lease_until=now()+lease
  WHERE id = (SELECT id FROM jobs WHERE status='queued' ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1)
  ```

  Concurrent workers never take the same job, and never wait for each other.
- **Heartbeat:** every few seconds the worker renews `lease_until` and stores progress. This only
  succeeds while the job is still the worker's. The heartbeat returns `cancel_requested`.
- **Reaper:** runs in every worker loop and when the API starts. A `running` job whose lease has
  expired goes back to `queued` if attempts remain, otherwise it becomes `failed` (the run gets a
  `WorkerLost` error).
- **Finalisation:** run status, hashes, metrics and job status are written in one transaction, and
  only if the job is still the worker's. A worker that lost its lease cannot overwrite the result of
  the worker that took over.

**Execution.** The worker runs every backtest in a freshly spawned child process: one engine per
process, never threads (PLAN §4.3). Parent and child talk over a pipe, and only the parent touches
the database.

- **Progress:** the child reports progress every `FINFORGE_PROGRESS_EVERY` engine events. The
  parent turns simulated time into a fraction of the run's window.
- **Cancel:** the parent passes cancel requests down, and the child raises the engine's cancel
  flag. The simulator stops between two events, open orders are cancelled, and the ledger is still
  reconciled. The run ends `cancelled`, with partial artefacts.
- **Worker death:** if the worker dies, the child notices at its next progress report (its parent
  has gone) and stops. The lease expires, and another worker retries or fails the job.

## 5. Trust boundary

- **Localhost only.** There is no authentication or user model (out of scope for v1): anyone who
  can reach the API can upload data and run backtests. Everything listens on 127.0.0.1 by default
  (`finforge server api --host`, the compose ports), and CORS allows only the local dashboard
  origins (`FINFORGE_CORS_ORIGINS`). Put an authenticating proxy in front before exposing it.
- **Uploads:** a request whose `Content-Length` exceeds the cap is refused with 413 before the body
  is read (411 without a `Content-Length`); the file's own bytes are checked against the cap again
  as they are written to disk.
  - Their type is **sniffed from the content** (zip / Parquet magic, JSON, comma-separated UTF-8
    text). Anything else is refused with 415.
  - Uploads are stored as `<uuid>.<ext>` under a directory the server chooses. The client's
    filename is sanitised (`../../etc/passwd` becomes `passwd`) and kept only as metadata.
- **Dataset specs** refer to uploads by id; there is no field for a path. Every request model
  rejects unknown fields.
- **Runs:**
  - Only built-in strategies run (D8). `module:Class` or file paths are refused.
  - Parameters are validated against the strategy's JSON Schema (no extra keys, types, ranges),
    then by the strategy's own checks (for example `fast < slow`).
  - The universe and data kind are checked against the dataset.
  - Clients cannot set output paths: the server builds the backtest config itself.
- **Errors:** strategy and data errors come back as 422 with the reason. A missing dataset or run
  is 404. Results of a run that has not finished are 409.

## 6. Tests

`python/tests/test_server.py` runs against a real PostgreSQL. CI uses a `postgres:16` service
container with `FINFORGE_REQUIRE_DB=1`, so these tests can never be skipped silently. Every test
gets a fresh, migrated database.

- **Workflow:** the scripted HTTP workflow covers two klines uploads, validate, ingest (and
  re-ingest, which is idempotent), two runs on a worker, the SSE stream from `queued` through
  `running` to `completed`, results, equity, trades, compare and listing.
- **Worker loss:** the worker is killed with SIGKILL during a run.
  - With one attempt, the job and run end `failed` (`WorkerLost`).
  - With two attempts, the job is re-queued and a second worker completes it.
  - In both cases the earlier run's row and files are byte-for-byte unchanged, and the orphaned
    backtest process stops by itself.
- **Cancel:** a queued run is cancelled immediately. A running run stops and ends `cancelled`
  (partial and reconciled), and a second cancel gets 409.
- **Trust boundary and docs:** the tests cover each refusal above, and check that OpenAPI is served
  at `/docs` with every route.
