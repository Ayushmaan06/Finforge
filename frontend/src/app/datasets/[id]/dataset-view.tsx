"use client";

import Link from "next/link";
import { use } from "react";

import { ErrorBox, Loading } from "@/components/ui";
import ValidationReport from "@/components/validation-report";
import { api, ok } from "@/lib/api";
import { int, isoDate, nsDate } from "@/lib/format";
import { useApi } from "@/lib/use-api";



export default function DatasetView({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  const ds = useApi(async () => ok(await api.GET("/datasets/{dataset_id}", { params: { path: { dataset_id: id } } })), id);
  if (ds.error) return <ErrorBox error={ds.error} />;
  if (!ds.data) return <Loading />;
  const d = ds.data;
  return (
    <>
      <h1>{d.name}</h1>
      <p className="muted">
        id {d.id} · schema v{d.schema_version} · {nsDate(d.time_first_ns)} → {nsDate(d.time_last_emit_ns)} · registered {isoDate(d.created_at)}
      </p>
      <p>
        <Link href={`/runs/new?dataset=${d.id}`}>
          <button>Backtest on this dataset</button>
        </Link>
      </p>
      <section className="panel">
        <h2>Instruments</h2>
        <table>
          <thead>
            <tr>
              <th>Id</th>
              <th>Symbol</th>
              <th>Class</th>
              <th>Quote</th>
              <th>Tick</th>
              <th>Lot</th>
              <th className="num">Min qty (lots)</th>
              <th className="num">Multiplier</th>
            </tr>
          </thead>
          <tbody>
            {d.instruments.map((i) => (
              <tr key={i.instrument_id}>
                <td>{i.instrument_id}</td>
                <td>{i.symbol}</td>
                <td>{i.asset_class}</td>
                <td>{i.quote_currency}</td>
                <td>{i.tick_size}</td>
                <td>{i.lot_size}</td>
                <td className="num">{i.min_qty}</td>
                <td className="num">{i.contract_multiplier}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>
      <section className="panel">
        <h2>Files</h2>
        <table>
          <thead>
            <tr>
              <th>Kind</th>
              <th>Symbol</th>
              <th className="num">Rows</th>
            </tr>
          </thead>
          <tbody>
            {d.files.map((f) => (
              <tr key={`${f.kind}-${f.symbol}`}>
                <td>{f.kind}</td>
                <td>{f.symbol}</td>
                <td className="num">{int(f.rows)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>
      <section className="panel">
        <h2>Validation</h2>
        <ValidationReport report={d.validation} />
      </section>
    </>
  );
}
