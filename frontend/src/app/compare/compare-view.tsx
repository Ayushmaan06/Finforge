"use client";

import Link from "next/link";
import { useRouter, useSearchParams } from "next/navigation";
import { useMemo, useState } from "react";

import TimeChart, { type Series } from "@/components/time-chart";
import { ErrorBox, Loading, tone } from "@/components/ui";
import { api, ok, type Compare } from "@/lib/api";
import { dd, int, num, pct, signedPct } from "@/lib/format";
import { useApi } from "@/lib/use-api";

const COLORS = ["#2f6fed", "#e8590c", "#16a34a", "#9333ea", "#0891b2", "#be123c", "#ca8a04", "#4b5563"];

export default function CompareView() {
  const router = useRouter();
  const search = useSearchParams();
  const ids = (search.get("runs") ?? "").split(",").filter(Boolean);
  const split = search.get("split") ?? "";
  const [splitInput, setSplitInput] = useState(split ? split.slice(0, 16) : "");
  const completed = useApi(async () => ok(await api.GET("/runs", { params: { query: { status: "completed", limit: 200 } } })));
  const key = `${ids.join(",")}|${split}`;
  const cmp = useApi<{ key: string; result?: Compare }>(
    async () =>
      ids.length === 0
        ? { key }
        : { key, result: ok(await api.GET("/compare", { params: { query: { runs: ids.join(","), ...(split ? { split } : {}) } } })) },
    key,
  );
  const data = cmp.data?.key === key ? cmp.data.result : undefined;
  const error = cmp.loading ? undefined : cmp.error;

  const go = (next: string[], s = split) => router.push(`/compare?runs=${next.join(",")}${s ? `&split=${encodeURIComponent(s)}` : ""}`);
  const toggle = (id: string) => go(ids.includes(id) ? ids.filter((x) => x !== id) : [...ids, id]);

  const { equity, drawdown } = useMemo(() => {
    if (!data) return { equity: [] as Series[], drawdown: [] as Series[] };
    const ts = data.series.ts as number[];
    const eq: Series[] = [];
    const ddS: Series[] = [];
    data.rows.forEach((row, i) => {
      const label = String(row.label);
      const color = COLORS[i % COLORS.length];
      eq.push({ name: label, color, data: ts.map((t, k) => ({ time: t, value: (data.series[`${label}_equity`] as number[])[k] })) });
      ddS.push({ name: label, color, data: ts.map((t, k) => ({ time: t, value: (data.series[`${label}_drawdown`] as number[])[k] })) });
    });
    return { equity: eq, drawdown: ddS };
  }, [data]);

  return (
    <>
      <section className="panel">
        <h2>Runs</h2>
        {completed.data ? (
          <div className="row" style={{ display: "flex", flexWrap: "wrap", gap: 12 }}>
            {completed.data.map((r) => (
              <label key={r.id} className="check">
                <input type="checkbox" checked={ids.includes(r.id)} onChange={() => toggle(r.id)} />
                {r.id.slice(0, 8)} {r.strategy} {JSON.stringify(r.params)}
              </label>
            ))}
          </div>
        ) : (
          <Loading />
        )}
        <form
          className="row"
          onSubmit={(e) => {
            e.preventDefault();
            go(ids, splitInput ? `${splitInput}:00Z` : "");
          }}
        >
          <label className="field">
            In-sample / out-of-sample split (UTC)
            <input type="datetime-local" value={splitInput} onChange={(e) => setSplitInput(e.target.value)} />
          </label>
          <button type="submit" className="secondary" style={{ alignSelf: "flex-end" }}>
            Apply split
          </button>
        </form>
      </section>

      {ids.length === 0 && <p className="muted">Pick completed runs to compare.</p>}
      <ErrorBox error={error} />
      {ids.length > 0 && !data && !error && <Loading />}

      {data && (
        <>
          <section className="panel scroll" aria-label="Comparison table">
            <table>
              <thead>
                <tr>
                  <th>Run</th>
                  <th>Params</th>
                  <th className="num">Return</th>
                  <th className="num">CAGR</th>
                  <th className="num">Vol</th>
                  <th className="num">Sharpe</th>
                  <th className="num">Sortino</th>
                  <th className="num">Max DD</th>
                  <th className="num">VaR95</th>
                  <th className="num">Trades</th>
                  <th className="num">Win</th>
                  <th className="num">Fees</th>
                  {split && (
                    <>
                      <th className="num">IS return</th>
                      <th className="num">IS Sharpe</th>
                      <th className="num">OOS return</th>
                      <th className="num">OOS Sharpe</th>
                    </>
                  )}
                </tr>
              </thead>
              <tbody>
                {data.rows.map((r, i) => (
                  <tr key={String(r.run_id)}>
                    <td>
                      <span style={{ color: COLORS[i % COLORS.length] }}>■</span>{" "}
                      <Link href={`/runs/${String(r.run_id)}`}>{String(r.run_id).slice(0, 8)}</Link> {String(r.strategy)}
                    </td>
                    <td className="muted">{JSON.stringify(r.params)}</td>
                    <td className={`num ${tone(r.total_return as number) ?? ""}`}>{signedPct(r.total_return as number)}</td>
                    <td className="num">{signedPct(r.cagr as number)}</td>
                    <td className="num">{pct(r.volatility as number)}</td>
                    <td className="num">{num(r.sharpe as number)}</td>
                    <td className="num">{r.sortino_infinite ? "∞" : num(r.sortino as number)}</td>
                    <td className="num">{dd(r.max_drawdown as number)}</td>
                    <td className="num">{pct(r.var as number)}</td>
                    <td className="num">{int(r.trades as number)}</td>
                    <td className="num">{pct(r.win_rate as number, 0)}</td>
                    <td className="num">{num(r.fees as number)}</td>
                    {split && (
                      <>
                        <td className="num">{signedPct(r.is_total_return as number)}</td>
                        <td className="num">{num(r.is_sharpe as number)}</td>
                        <td className="num">{signedPct(r.oos_total_return as number)}</td>
                        <td className="num">{num(r.oos_sharpe as number)}</td>
                      </>
                    )}
                  </tr>
                ))}
              </tbody>
            </table>
          </section>
          <section className="panel">
            <h2>Equity (growth of 1, daily)</h2>
            <TimeChart series={equity} height={300} testId="compare-equity" />
          </section>
          <section className="panel">
            <h2>Drawdown</h2>
            <TimeChart series={drawdown} height={220} percent testId="compare-drawdown" />
          </section>
        </>
      )}
    </>
  );
}
