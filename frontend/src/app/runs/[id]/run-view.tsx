"use client";

import Link from "next/link";
import { use, useEffect, useMemo, useRef, useState } from "react";

import Histogram from "@/components/histogram";
import TimeChart, { preloadCharts, type Series } from "@/components/time-chart";
import { Card, ErrorBox, Loading, Status, tone } from "@/components/ui";
import { api, eventsUrl, ok, type Equity, type Prices, type Returns, type Run } from "@/lib/api";
import { dd, duration, int, isoDate, num, pct, signedPct } from "@/lib/format";
import { useApi } from "@/lib/use-api";

type Report = {
  performance: Record<string, unknown> & { drawdown: Record<string, unknown> };
  trades: Record<string, unknown> & { holding_period?: Record<string, number> | null };
  exposure: Record<string, number | null>;
  instruments: Record<string, Record<string, number>>;
  benchmark: { performance: Record<string, number | null>; excess_total_return: number } | null;
  conventions: Record<string, unknown>;
};

const TERMINAL = ["completed", "failed", "cancelled"];

export default function RunView({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  useEffect(() => {
    preloadCharts(); // ready by the time the run completes
  }, []);
  const runQ = useApi(async () => ok(await api.GET("/runs/{run_id}", { params: { path: { run_id: id } } })), id);
  const [error, setError] = useState<Error>();
  // Latest server-sent event: progress and status while the run is live.
  const [live, setLive] = useState<{ status: Run["status"]; progress: number; events: number } | null>(null);
  const fetched = runQ.data;
  const finished = fetched ? TERMINAL.includes(fetched.status) : true;
  const reload = runQ.reload;

  // Live progress over server-sent events until the run is finished; then reload the run.
  useEffect(() => {
    if (finished) return;
    const es = new EventSource(eventsUrl(id));
    const onData = (e: MessageEvent) => {
      const d = JSON.parse(e.data);
      setLive({ status: d.status, progress: d.progress ?? 0, events: d.events_processed ?? 0 });
    };
    es.addEventListener("status", onData);
    es.addEventListener("progress", onData);
    es.addEventListener("end", () => {
      es.close();
      reload();
    });
    es.onerror = () => {
      es.close();
      setTimeout(reload, 1000);
    };
    return () => es.close();
  }, [id, finished, reload]);

  async function cancel() {
    try {
      ok(await api.POST("/runs/{run_id}/cancel", { params: { path: { run_id: id } } }));
      reload();
    } catch (e) {
      setError(e as Error);
    }
  }

  if (runQ.error && !fetched) return <ErrorBox error={runQ.error} />;
  if (!fetched) return <Loading />;
  const run: Run = !finished && live ? { ...fetched, status: live.status } : fetched;
  const progress = run.status === "completed" ? 1 : (live?.progress ?? run.job.progress);

  return (
    <>
      <h1>
        Run {id.slice(0, 8)} <Status status={run.status} />
      </h1>
      <p className="muted">
        {run.strategy} {JSON.stringify(run.params)} · dataset <Link href={`/datasets/${run.dataset_id}`}>{run.dataset_id.slice(0, 8)}</Link> ·{" "}
        {String((run.config.execution as Record<string, unknown>).model)} model · created {isoDate(run.created_at)}
        {run.wall_seconds !== null && ` · ${num(run.wall_seconds, 1)} s`}
      </p>
      <ErrorBox error={error} />

      {!TERMINAL.includes(run.status) && (
        <section className="panel" aria-label="Progress">
          <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 8 }}>
            <span>
              {run.status === "queued" ? "Waiting for a worker…" : `Running: ${pct(progress, 0)}`}
              {live && live.events > 0 ? ` · ${int(live.events)} events` : ""}
            </span>
            <button className="secondary" onClick={cancel} disabled={run.job.cancel_requested}>
              Cancel run
            </button>
          </div>
          <div className="progress" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(progress * 100)}>
            <div style={{ width: `${progress * 100}%` }} />
          </div>
        </section>
      )}

      {run.status === "failed" && run.error && (
        <section className="panel">
          <h2>Failure</h2>
          <ErrorBox error={`${String(run.error.type)}: ${String(run.error.message)}`} />
          {typeof run.error.traceback === "string" && <pre>{run.error.traceback}</pre>}
        </section>
      )}

      {run.status === "completed" && <Results run={run} />}
    </>
  );
}

function Results({ run }: { run: Run }) {
  const id = run.id;
  const [report, setReport] = useState<Report>();
  const [equity, setEquity] = useState<Equity>();
  const [returns, setReturns] = useState<Returns>();
  const [error, setError] = useState<Error>();
  const fetchStart = useRef(0);
  const [renderMs, setRenderMs] = useState<number | null>(null);

  useEffect(() => {
    fetchStart.current = performance.now();
    preloadCharts(); // in parallel with the data
    const p = { params: { path: { run_id: id } } };
    // The equity curve first, alone, so the primary chart is not slowed by the other requests;
    // the report and the returns follow.
    api
      .GET("/runs/{run_id}/equity", { params: { path: { run_id: id }, query: { max_points: 2000 } } })
      .then((r) => {
        setEquity(ok(r));
        api.GET("/runs/{run_id}/results", p).then((r2) => setReport(ok(r2) as unknown as Report)).catch(setError);
        api.GET("/runs/{run_id}/returns", p).then((r2) => setReturns(ok(r2))).catch(setError);
      })
      .catch(setError);
  }, [id]);

  const equitySeries = useMemo<Series[]>(() => {
    if (!equity) return [];
    const s: Series[] = [{ name: "Strategy", color: "#2f6fed", data: equity.ts.map((t, i) => ({ time: t, value: equity.equity[i] })) }];
    if (equity.benchmark) {
      s.push({
        name: "Buy and hold",
        color: "#98a1b3",
        data: equity.ts.flatMap((t, i) => (equity.benchmark![i] === null ? [] : [{ time: t, value: equity.benchmark![i] as number }])),
      });
    }
    return s;
  }, [equity]);
  const drawdownSeries = useMemo<Series[]>(
    () => (equity ? [{ name: "Drawdown", color: "#dc2626", kind: "area", data: equity.ts.map((t, i) => ({ time: t, value: equity.drawdown[i] })) }] : []),
    [equity],
  );

  const perf = report?.performance;
  const t = report?.trades;
  const bench = report?.benchmark;
  const m = run.metrics;

  return (
    <>
      <ErrorBox error={error} />
      <div className="cards" style={{ marginBottom: 16 }} aria-label="Metrics">
        <Card label="Total return" value={signedPct(m?.total_return)} tone={tone(m?.total_return)} />
        <Card label="Benchmark return" value={signedPct(m?.benchmark_return)} tone={tone(m?.benchmark_return)} />
        <Card label="Excess return" value={signedPct(bench?.excess_total_return)} tone={tone(bench?.excess_total_return)} />
        <Card label="CAGR" value={signedPct(m?.cagr)} />
        <Card label="Volatility" value={pct(m?.volatility)} />
        <Card label="Sharpe" value={num(m?.sharpe)} />
        <Card label="Sortino" value={num(m?.sortino)} />
        <Card label="Max drawdown" value={dd(m?.max_drawdown)} tone={m?.max_drawdown ? "bad" : undefined} />
        <Card label="VaR 95% (1d)" value={pct(m?.var)} />
        <Card label="CVaR 95% (1d)" value={pct(m?.cvar)} />
        <Card label="Trades" value={int(m?.trades)} />
        <Card label="Win rate" value={pct(m?.win_rate, 0)} />
        <Card label="Profit factor" value={t?.profit_factor_infinite ? "∞" : num(m?.profit_factor)} />
        <Card label="Fees" value={num(m?.fees)} />
      </div>

      <section className="panel">
        <h2>Equity</h2>
        {equity ? (
          <>
            <TimeChart
              series={equitySeries}
              height={320}
              testId="equity-chart"
              onRendered={() => setRenderMs(performance.now() - fetchStart.current)}
            />
            <p className="muted" style={{ fontSize: 12 }} data-testid="equity-render" data-render-ms={renderMs ?? ""}>
              {int(equity.equity.length)} of {int(equity.points_total)} points (LTTB)
              {renderMs !== null && ` · loaded and drawn in ${Math.round(renderMs)} ms`}
            </p>
          </>
        ) : (
          <Loading />
        )}
      </section>

      <div className="grid cols-2">
        <section className="panel">
          <h2>Drawdown</h2>
          {equity ? <TimeChart series={drawdownSeries} height={220} percent testId="drawdown-chart" /> : <Loading />}
        </section>
        <section className="panel">
          <h2>Daily returns</h2>
          {returns ? <Histogram values={returns.returns} compare={returns.benchmark ?? undefined} label="Distribution of daily returns" /> : <Loading />}
        </section>
      </div>

      {equity && <PriceChart run={run} />}

      {report && (
        <div className="grid cols-2">
          <section className="panel">
            <h2>Execution</h2>
            <table>
              <tbody>
                <tr><td>Fees</td><td className="num">{num(t!.fees as number)}</td></tr>
                <tr><td>Price cost (slippage / spread vs reference)</td><td className="num">{num(t!.price_cost as number)}</td></tr>
                <tr><td>Execution cost (fees + price cost)</td><td className="num">{num(t!.execution_cost as number)}</td></tr>
                <tr><td>Gross profit / loss</td><td className="num">{num(t!.gross_profit as number)} / {num(t!.gross_loss as number)}</td></tr>
                <tr><td>Average net P&amp;L per trade</td><td className="num">{num(t!.average_net_pnl as number)}</td></tr>
                <tr><td>Holding period (median / mean)</td><td className="num">{duration(t!.holding_period?.median_s)} / {duration(t!.holding_period?.mean_s)}</td></tr>
                <tr><td>Open lots at the end</td><td className="num">{int(t!.open_lots as number)}</td></tr>
              </tbody>
            </table>
          </section>
          <section className="panel">
            <h2>Risk and exposure</h2>
            <table>
              <tbody>
                <tr><td>Max drawdown period</td><td className="num">{duration(((perf!.drawdown.duration_ns as number) ?? 0) / 1e9)} {perf!.drawdown.recovered ? "(recovered)" : "(not recovered)"}</td></tr>
                <tr><td>Longest time under water</td><td className="num">{duration(((perf!.drawdown.longest_underwater_ns as number) ?? 0) / 1e9)}</td></tr>
                <tr><td>Calmar</td><td className="num">{num(perf!.calmar as number)}</td></tr>
                <tr><td>Time in market</td><td className="num">{pct(report.exposure.time_in_market, 1)}</td></tr>
                <tr><td>Mean / max capital utilisation</td><td className="num">{pct(report.exposure.mean_utilisation, 1)} / {pct(report.exposure.max_utilisation, 1)}</td></tr>
                <tr><td>Reconciles (ACC-10/11)</td><td className="num">{run.reconciles ? "yes" : "no"}</td></tr>
                <tr><td>Journal hash</td><td className="num muted">{run.journal_hash?.slice(0, 16)}</td></tr>
              </tbody>
            </table>
            <p className="muted" style={{ fontSize: 12 }}>
              Conventions: {Object.entries(report.conventions).map(([k, v]) => `${k} ${String(v)}`).join(" · ")}
              {Array.isArray(perf!.undefined) && (perf!.undefined as string[]).length > 0 && ` · undefined: ${(perf!.undefined as string[]).join(", ")}`}
            </p>
          </section>
        </div>
      )}

      <Trades id={id} />
      <p>
        <Link href={`/compare?runs=${id}`}>Compare this run with others →</Link> ·{" "}
        <Link href={`/runs/new?dataset=${run.dataset_id}&strategy=${run.strategy}`}>New run with another parameter set →</Link>
      </p>
    </>
  );
}

function PriceChart({ run }: { run: Run }) {
  const universe = run.config.universe as string[];
  const [symbol, setSymbol] = useState(universe[0]);
  const q = useApi(
    async () => ok(await api.GET("/runs/{run_id}/prices", { params: { path: { run_id: run.id }, query: { symbol, max_points: 2000, max_markers: 1000 } } })),
    `${run.id}:${symbol}`,
  );
  const prices: Prices | undefined = q.data?.symbol === symbol ? q.data : undefined;
  const error = q.error;
  const series = useMemo<Series[]>(
    () => (prices ? [{ name: `${symbol} ${prices.kind === "bars" ? "close" : "mid"}`, color: "#667085", data: prices.ts.map((t, i) => ({ time: t, value: prices.price[i] })) }] : []),
    [prices, symbol],
  );
  const markers = useMemo(() => (prices ? { series: 0, items: prices.markers.map((m) => ({ time: m.ts, side: m.side })) } : undefined), [prices]);
  return (
    <section className="panel">
      <div style={{ display: "flex", justifyContent: "space-between" }}>
        <h2>Price and fills</h2>
        {universe.length > 1 && (
          <select aria-label="Instrument" value={symbol} onChange={(e) => setSymbol(e.target.value)}>
            {universe.map((s) => (
              <option key={s}>{s}</option>
            ))}
          </select>
        )}
      </div>
      <ErrorBox error={error} />
      {prices ? (
        <>
          <TimeChart series={series} markers={markers} height={300} testId="price-chart" />
          <p className="muted" style={{ fontSize: 12 }}>
            {int(prices.markers.length)} of {int(prices.markers_total)} fills shown (▲ buy, ▼ sell)
          </p>
        </>
      ) : (
        <Loading />
      )}
    </section>
  );
}

function Trades({ id }: { id: string }) {
  const PAGE = 50;
  const [offset, setOffset] = useState(0);
  const [data, setData] = useState<{ total: number; trades: Record<string, unknown>[] }>();
  useEffect(() => {
    api.GET("/runs/{run_id}/trades", { params: { path: { run_id: id }, query: { offset, limit: PAGE } } }).then((r) => setData(ok(r)));
  }, [id, offset]);
  if (!data) return null;
  return (
    <section className="panel" aria-label="Trades">
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
        <h2>Trades ({int(data.total)} round trips, FIFO)</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="secondary" disabled={offset === 0} onClick={() => setOffset(Math.max(0, offset - PAGE))}>
            Previous
          </button>
          <button className="secondary" disabled={offset + PAGE >= data.total} onClick={() => setOffset(offset + PAGE)}>
            Next
          </button>
        </div>
      </div>
      <div className="scroll">
        <table>
          <thead>
            <tr>
              <th>Symbol</th>
              <th>Side</th>
              <th>Entry</th>
              <th>Exit</th>
              <th className="num">Qty</th>
              <th className="num">Entry px</th>
              <th className="num">Exit px</th>
              <th className="num">Net P&amp;L</th>
              <th className="num">Fees</th>
              <th className="num">Held</th>
            </tr>
          </thead>
          <tbody>
            {data.trades.map((t, i) => (
              <tr key={i}>
                <td>{String(t.symbol)}</td>
                <td>{String(t.direction)}</td>
                <td className="muted">{new Date((t.entry_ts as number) / 1e6).toISOString().slice(0, 16).replace("T", " ")}</td>
                <td className="muted">{new Date((t.exit_ts as number) / 1e6).toISOString().slice(0, 16).replace("T", " ")}</td>
                <td className="num">{num(t.qty as number, 4)}</td>
                <td className="num">{num(t.entry_price as number)}</td>
                <td className="num">{num(t.exit_price as number)}</td>
                <td className={`num ${tone(t.net_pnl as number) ?? ""}`}>{num(t.net_pnl as number)}</td>
                <td className="num">{num(t.fees as number)}</td>
                <td className="num">{duration((t.duration_ns as number) / 1e9)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
