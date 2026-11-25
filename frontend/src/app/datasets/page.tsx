"use client";

import Link from "next/link";
import { useState } from "react";

import { ErrorBox, Loading } from "@/components/ui";
import ValidationReport from "@/components/validation-report";
import { api, ok, uploadFile, type DatasetRequest } from "@/lib/api";
import { int, nsDate } from "@/lib/format";
import { useApi } from "@/lib/use-api";

type SourceRow = {
  symbol: string;
  quote_currency: string;
  tick_size: string;
  lot_size: string;
  format: "binance_klines" | "binance_bookticker";
  interval: string;
  file?: File;
};

const EMPTY: SourceRow = { symbol: "", quote_currency: "USDT", tick_size: "0.01", lot_size: "0.001", format: "binance_klines", interval: "1m" };

export default function DatasetsPage() {
  const list = useApi(async () => ok(await api.GET("/datasets")));
  const [name, setName] = useState("");
  const [rows, setRows] = useState<SourceRow[]>([{ ...EMPTY }]);
  const [report, setReport] = useState<Record<string, unknown> | null>(null);
  const [error, setError] = useState<Error | undefined>();
  const [busy, setBusy] = useState(false);
  const [created, setCreated] = useState<string | null>(null);

  const set = (i: number, patch: Partial<SourceRow>) => setRows((rs) => rs.map((r, k) => (k === i ? { ...r, ...patch } : r)));

  async function buildSpec(): Promise<DatasetRequest> {
    const sources = [];
    for (const r of rows) {
      if (!r.file) throw new Error(`choose a file for ${r.symbol || "each instrument"}`);
      const up = await uploadFile(r.file);
      sources.push({ format: r.format, symbol: r.symbol, uploads: [up.id], ...(r.format === "binance_klines" ? { interval: r.interval } : {}) });
    }
    return {
      name,
      instruments: rows.map((r) => ({ symbol: r.symbol, quote_currency: r.quote_currency, tick_size: r.tick_size, lot_size: r.lot_size })),
      sources,
    } as DatasetRequest;
  }

  async function submit(validateOnly: boolean) {
    setBusy(true);
    setError(undefined);
    setReport(null);
    setCreated(null);
    try {
      const spec = await buildSpec();
      if (validateOnly) {
        setReport(ok(await api.POST("/datasets/validate", { body: spec })) as Record<string, unknown>);
      } else {
        const d = ok(await api.POST("/datasets", { body: spec }));
        setCreated(d.id);
        setReport(d.validation);
        list.reload();
      }
    } catch (e) {
      setError(e as Error);
    } finally {
      setBusy(false);
    }
  }

  return (
    <>
      <h1>Datasets</h1>
      <section className="panel">
        <ErrorBox error={list.error} />
        {list.loading && !list.data ? (
          <Loading />
        ) : (
          <table>
            <thead>
              <tr>
                <th>Name</th>
                <th>Files</th>
                <th>From</th>
                <th>To</th>
                <th>Validation</th>
              </tr>
            </thead>
            <tbody>
              {(list.data ?? []).map((d) => (
                <tr key={d.id}>
                  <td>
                    <Link href={`/datasets/${d.id}`}>{d.name}</Link>
                  </td>
                  <td>{d.files.map((f) => `${f.symbol} ${f.kind} (${int(f.rows)})`).join(", ")}</td>
                  <td>{nsDate(d.time_first_ns)}</td>
                  <td>{nsDate(d.time_last_emit_ns)}</td>
                  <td>{d.validation_status}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>

      <section className="panel">
        <h2>New dataset</h2>
        <p className="muted">
          Upload one Binance kline or bookTicker file (CSV or the original zip) per instrument. Prices and quantities are converted
          exactly to the tick and lot sizes you give; the validation report lists every problem found.
        </p>
        <form onSubmit={(e) => (e.preventDefault(), submit(false))}>
          <div className="row">
            <label className="field">
              Dataset name
              <input value={name} onChange={(e) => setName(e.target.value)} required pattern="[A-Za-z0-9._ \-]{1,120}" />
            </label>
          </div>
          {rows.map((r, i) => (
            <div className="row" key={i}>
              <label className="field">
                Symbol
                <input value={r.symbol} onChange={(e) => set(i, { symbol: e.target.value })} required />
              </label>
              <label className="field">
                Quote currency
                <input value={r.quote_currency} onChange={(e) => set(i, { quote_currency: e.target.value })} required />
              </label>
              <label className="field">
                Tick size
                <input value={r.tick_size} onChange={(e) => set(i, { tick_size: e.target.value })} required />
              </label>
              <label className="field">
                Lot size
                <input value={r.lot_size} onChange={(e) => set(i, { lot_size: e.target.value })} required />
              </label>
              <label className="field">
                Format
                <select value={r.format} onChange={(e) => set(i, { format: e.target.value as SourceRow["format"] })}>
                  <option value="binance_klines">Binance klines (bars)</option>
                  <option value="binance_bookticker">Binance bookTicker (quotes)</option>
                </select>
              </label>
              {r.format === "binance_klines" && (
                <label className="field">
                  Interval
                  <input value={r.interval} onChange={(e) => set(i, { interval: e.target.value })} required />
                </label>
              )}
              <label className="field">
                File
                <input type="file" accept=".csv,.zip" onChange={(e) => set(i, { file: e.target.files?.[0] })} required />
              </label>
            </div>
          ))}
          <div className="row">
            <button type="button" className="secondary" onClick={() => setRows((rs) => [...rs, { ...EMPTY }])}>
              Add instrument
            </button>
            <button type="button" className="secondary" disabled={busy} onClick={() => submit(true)}>
              Validate
            </button>
            <button type="submit" disabled={busy}>
              Create dataset
            </button>
          </div>
        </form>
        <ErrorBox error={error} />
        {created && (
          <p>
            Created <Link href={`/datasets/${created}`}>dataset {created.slice(0, 12)}</Link>.{" "}
            <Link href={`/runs/new?dataset=${created}`}>Backtest on it →</Link>
          </p>
        )}
        {report && <ValidationReport report={report} />}
      </section>
    </>
  );
}
