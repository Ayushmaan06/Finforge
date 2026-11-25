# Research dashboard

A Next.js + TypeScript app in `frontend/` that runs the whole primary workflow, Demo 1, in the
browser (PLAN.md Phase 8): pick a dataset → MA crossover → set parameters, capital, fees, slippage
and model → start → watch progress → inspect equity against the benchmark, trades, drawdown and
risk → compare with a second parameter set. It only talks to the API ([server.md](server.md)) and
contains no simulation logic.

## 1. Running it

```bash
# backend (see server.md), with CORS allowing the dashboard's origin (default :3000)
finforge server migrate && finforge server api & finforge server worker &
# or, for a self-contained demo: Postgres (pgserver), API, worker and a one-year demo dataset
python scripts/e2e_stack.py --port 8000

cd frontend
npm ci
NEXT_PUBLIC_API_URL=http://127.0.0.1:8000 npm run dev    # http://localhost:3000
```

`NEXT_PUBLIC_API_URL` is inlined at build time. For a production build, set it when running
`npm run build`.

## 2. Pages

| Page | What it does |
|---|---|
| `/` Overview | counts, recent runs with headline metrics (select some to compare), datasets |
| `/datasets` | list; **new dataset**: upload one Binance kline or bookTicker file per instrument, set tick and lot sizes, *Validate* (report only) or *Create* |
| `/datasets/[id]` | metadata, instruments, files, validation report |
| `/strategies`, `/strategies/[name]` | built-in strategies, their documentation (docstring) and parameter table (from the JSON Schema) |
| `/runs/new` | backtest form: dataset, model, UTC date range, universe; strategy parameters **generated from the strategy's JSON Schema**; capital, slippage, fees, leverage, shorts, seed, benchmark |
| `/runs/[id]` | live progress (SSE) with cancel; when completed: metric cards, equity against buy-and-hold, drawdown, histogram of daily returns, price with trade markers, execution statistics, risk and exposure, conventions, FIFO trades table |
| `/compare` | pick runs; metrics table; overlaid equity (growth of 1) and drawdown; an optional in-sample / out-of-sample split adds IS and OOS return and Sharpe columns |

## 3. Design decisions

- **Charts: TradingView `lightweight-charts`** for every time series, the price chart with buy and
  sell markers included. This is the decision PLAN Phase 8 asked to log. It draws on canvas, is
  built for financial time series, and draws a few thousand points in a frame. Recharts would have
  added a large dependency tree for the one chart that is not a time series. That chart, the
  returns histogram, is about 50 lines of SVG (`src/components/histogram.tsx`).
- **Downsampling on the server.** The equity and price endpoints take `max_points` and return
  min/max per bucket (`finforge.analytics.downsample`).
  - Each bucket keeps its lowest and highest point, so peaks and troughs survive exactly.
  - The method is one vectorised pass: a one-year minute curve (525 600 points) reduces in about
    20 ms. LTTB is also available; it took about 40–75 ms here.
  - The drawdown is computed on the full curve **before** downsampling, and the max-drawdown and
    peak points are always kept, so the chart and the metric agree.
  - Fill times are kept in the price series, so markers sit on real points.
- **Typed client generated from OpenAPI.** The API got Pydantic response models, so its OpenAPI
  document is precise. `npm run gen:api` runs `scripts/dump_openapi.py` and
  `openapi-typescript` to produce `src/lib/api-schema.d.ts`. `openapi-fetch` then checks every path,
  parameter and response shape at compile time. CI regenerates the file and fails if it differs.
- **Client components and data loading.**
  - Pages fetch from the API in the browser (`useApi`, keyed by its inputs; no state is synchronised
    inside effects).
  - Dynamic routes and pages that read search params stream their client view inside `<Suspense>`,
    as Next.js 16 with Cache Components requires (a blocking route fails the build).
- **Primary chart first.** The run page fetches the equity curve alone, and asks for the report,
  returns and prices once it has arrived. The chart library starts loading when the page mounts.

## 4. Performance: one year of 1-minute equity in under 1 s

The run page records the time from the start of the equity request until the chart has been drawn
(the next animation frame). It exposes this as `data-render-ms`, and the end-to-end test asserts
that it is below 1000 ms for a 525 600-point equity curve.

| Change | Fetched and drawn |
|---|---|
| first version (four requests at once, chart library loaded after the data, LTTB) | 853, 881 ms |
| chart library preloaded | 663, 881 ms |
| equity requested first, alone; min/max downsampling | 232, 238, 383, 359 ms |

The bottleneck was contention: four requests at once in one API process share the GIL. The equity
endpoint took 222 ms alone and 332 ms alongside the others.

## 5. End-to-end test

`frontend/e2e/demo.spec.ts` (Playwright, Chromium) runs Demo 1:

1. **Dataset:** uploads a fixture file on the Datasets page, validates it (expects "clean") and
   creates a dataset.
2. **Strategy docs:** opens the MA crossover documentation.
3. **First run:** starts a backtest on the one-year demo dataset from the form and sees live
   progress until the run completes.
4. **Results:**
   - checks the equity chart is drawn in under 1 s from 525 600 points, with the buy-and-hold line;
   - checks the drawdown, price and histogram charts, the metric cards, the trades table and the
     execution statistics.
5. **Second run:** starts a run with a second parameter set.
6. **Compare:** compares the two runs (2 rows, overlaid equity) and applies an IS/OOS split (OOS
   columns appear).

Playwright's `webServer` starts both servers:

- **Backend:** `scripts/e2e_stack.py` runs Postgres (`FINFORGE_DATABASE_URL`, or a throwaway
  pgserver instance), migrations, the demo dataset (`finforge.data.demo`, 525 600 bars, built in
  about 3 s), a worker and the API.
- **Dashboard:** a production build served by `next start`.

On SIGTERM both shut down cleanly, and the backend stops its own Postgres.

Selectors use roles or explicit visibility filters. With Cache Components, Next.js keeps visited
routes mounted but hidden (React Activity), so plain CSS or label queries can match hidden copies.

In CI the **Dashboard** job runs lint, typecheck, the OpenAPI drift check and the Playwright test
against a `postgres:16` service.
