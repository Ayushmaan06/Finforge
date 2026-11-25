"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useState } from "react";

import { Card, ErrorBox, Loading, Status, tone } from "@/components/ui";
import { api, ok } from "@/lib/api";
import { dd, isoDate, num, signedPct } from "@/lib/format";
import { useApi } from "@/lib/use-api";

export default function Overview() {
  const router = useRouter();
  const runs = useApi(async () => ok(await api.GET("/runs", { params: { query: { limit: 50 } } })));
  const datasets = useApi(async () => ok(await api.GET("/datasets")));
  const [picked, setPicked] = useState<string[]>([]);

  const counts = (runs.data ?? []).reduce<Record<string, number>>((acc, r) => ((acc[r.status] = (acc[r.status] ?? 0) + 1), acc), {});
  const toggle = (id: string) => setPicked((p) => (p.includes(id) ? p.filter((x) => x !== id) : [...p, id]));

  return (
    <>
      <h1>Overview</h1>
      <div className="cards" style={{ marginBottom: 16 }}>
        <Card label="Datasets" value={String(datasets.data?.length ?? "–")} />
        <Card label="Runs" value={String(runs.data?.length ?? "–")} />
        <Card label="Completed" value={String(counts.completed ?? 0)} />
        <Card label="Running / queued" value={String((counts.running ?? 0) + (counts.queued ?? 0))} />
        <Card label="Failed / cancelled" value={String((counts.failed ?? 0) + (counts.cancelled ?? 0))} />
      </div>

      <section className="panel">
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
          <h2>Recent runs</h2>
          <div style={{ display: "flex", gap: 8 }}>
            <button className="secondary" disabled={picked.length < 1} onClick={() => router.push(`/compare?runs=${picked.join(",")}`)}>
              Compare selected ({picked.length})
            </button>
            <Link href="/runs/new">
              <button>New backtest</button>
            </Link>
          </div>
        </div>
        <ErrorBox error={runs.error} />
        {runs.loading && !runs.data ? (
          <Loading />
        ) : (
          <div className="scroll">
            <table>
              <thead>
                <tr>
                  <th />
                  <th>Run</th>
                  <th>Status</th>
                  <th>Strategy</th>
                  <th>Params</th>
                  <th className="num">Return</th>
                  <th className="num">Sharpe</th>
                  <th className="num">Max DD</th>
                  <th>Created</th>
                </tr>
              </thead>
              <tbody>
                {(runs.data ?? []).map((r) => (
                  <tr key={r.id}>
                    <td>
                      <input
                        type="checkbox"
                        aria-label={`select run ${r.id.slice(0, 8)}`}
                        disabled={r.status !== "completed"}
                        checked={picked.includes(r.id)}
                        onChange={() => toggle(r.id)}
                      />
                    </td>
                    <td>
                      <Link href={`/runs/${r.id}`}>{r.id.slice(0, 8)}</Link>
                    </td>
                    <td>
                      <Status status={r.status} />
                    </td>
                    <td>{r.strategy}</td>
                    <td className="muted">{JSON.stringify(r.params)}</td>
                    <td className={`num ${tone(r.metrics?.total_return) ?? ""}`}>{signedPct(r.metrics?.total_return)}</td>
                    <td className="num">{num(r.metrics?.sharpe)}</td>
                    <td className="num">{dd(r.metrics?.max_drawdown)}</td>
                    <td className="muted">{isoDate(r.created_at)}</td>
                  </tr>
                ))}
                {runs.data?.length === 0 && (
                  <tr>
                    <td colSpan={9} className="muted">
                      No runs yet. <Link href="/runs/new">Start a backtest.</Link>
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        )}
      </section>

      <section className="panel">
        <h2>Datasets</h2>
        <ErrorBox error={datasets.error} />
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Files</th>
              <th>Validation</th>
              <th>Id</th>
            </tr>
          </thead>
          <tbody>
            {(datasets.data ?? []).map((d) => (
              <tr key={d.id}>
                <td>
                  <Link href={`/datasets/${d.id}`}>{d.name}</Link>
                </td>
                <td>{d.files.map((f) => `${f.symbol} ${f.kind}`).join(", ")}</td>
                <td>{d.validation_status}</td>
                <td className="muted">{d.id.slice(0, 12)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>
    </>
  );
}
