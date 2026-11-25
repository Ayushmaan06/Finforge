"use client";

import { useRouter, useSearchParams } from "next/navigation";
import { useMemo, useState } from "react";

import SchemaForm, { coerceParams, type ParamSchema } from "@/components/schema-form";
import { ErrorBox, Loading } from "@/components/ui";
import { api, ok, type RunRequest } from "@/lib/api";
import { useApi } from "@/lib/use-api";

type Params = Record<string, string | boolean>;

function initialParams(defaults: Record<string, unknown>): Params {
  return Object.fromEntries(Object.entries(defaults).map(([k, v]) => [k, typeof v === "boolean" ? v : String(v)]));
}

export default function NewBacktest() {
  const router = useRouter();
  const search = useSearchParams();
  const datasets = useApi(async () => ok(await api.GET("/datasets")));
  const strategies = useApi(async () => ok(await api.GET("/strategies")));

  const [chosenDataset, setDatasetId] = useState(search.get("dataset") ?? "");
  const [strategy, setStrategy] = useState(search.get("strategy") ?? "ma_cross");
  // Parameters and universe are remembered per strategy / per dataset and model.
  const [paramsBy, setParamsBy] = useState<Record<string, Params>>({});
  const [universeBy, setUniverseBy] = useState<Record<string, string[]>>({});
  const [form, setForm] = useState({
    start: "",
    end: "",
    capital: "10000",
    model: "candle" as "candle" | "quote",
    slippageKind: "fixed_bps" as "none" | "fixed_bps" | "half_spread",
    slippageBps: "1",
    makerBps: "2",
    takerBps: "10",
    minFee: "0",
    allowShort: false,
    maxLeverage: "1",
    seed: "0",
    benchmark: true,
  });
  const [error, setError] = useState<Error | undefined>();
  const [busy, setBusy] = useState(false);

  const strategyInfo = useApi(
    async () => ok(await api.GET("/strategies/{name}", { params: { path: { name: strategy } } })),
    strategy,
  );
  const info = strategyInfo.data?.name === strategy ? strategyInfo.data : undefined;
  const params: Params = paramsBy[strategy] ?? (info ? initialParams(info.defaults) : {});
  const setParams = (v: Params) => setParamsBy((p) => ({ ...p, [strategy]: v }));

  // Default: the most recently registered dataset.
  const datasetId = chosenDataset || (datasets.data?.length ? datasets.data[datasets.data.length - 1].id : "");
  const dataset = datasets.data?.find((d) => d.id === datasetId);
  const kind = form.model === "candle" ? "bars" : "quotes";
  const symbols = useMemo(() => (dataset?.files ?? []).filter((f) => f.kind === kind).map((f) => f.symbol), [dataset, kind]);
  const universeKey = `${datasetId}:${kind}`;
  const universe = universeBy[universeKey] ?? symbols.slice(0, 1);
  const setUniverse = (f: (u: string[]) => string[]) => setUniverseBy((by) => ({ ...by, [universeKey]: f(universe) }));

  const set = <K extends keyof typeof form>(k: K, v: (typeof form)[K]) => setForm((f) => ({ ...f, [k]: v }));

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError(undefined);
    try {
      const body = {
        dataset_id: datasetId,
        universe,
        start: form.start ? `${form.start}:00Z` : null,
        end: form.end ? `${form.end}:00Z` : null,
        initial_capital: form.capital,
        execution: {
          model: form.model,
          slippage: form.model === "candle" ? { kind: form.slippageKind, bps: form.slippageKind === "none" ? "0" : form.slippageBps } : { kind: "none", bps: "0" },
        },
        fees: { maker_bps: form.makerBps, taker_bps: form.takerBps, min_fee: form.minFee },
        risk: { allow_short: form.allowShort, max_leverage: form.maxLeverage },
        seed: parseInt(form.seed, 10) || 0,
        strategy: { name: strategy, params: coerceParams((info?.params_schema ?? {}) as ParamSchema, params) },
        benchmark: form.benchmark,
      } as unknown as RunRequest;
      const run = ok(await api.POST("/runs", { body }));
      router.push(`/runs/${run.id}`);
    } catch (err) {
      setError(err as Error);
      setBusy(false);
    }
  }

  if (datasets.error) return <ErrorBox error={datasets.error} />;
  if (!datasets.data || !strategies.data) return <Loading />;
  if (datasets.data.length === 0) return <p>No datasets yet: create one on the Datasets page.</p>;

  return (
    <form onSubmit={submit}>
      <section className="panel">
        <h2>Data</h2>
        <div className="row">
          <label className="field">
            Dataset
            <select value={datasetId} onChange={(e) => setDatasetId(e.target.value)}>
              {datasets.data.map((d) => (
                <option key={d.id} value={d.id}>
                  {d.name} ({d.id.slice(0, 8)})
                </option>
              ))}
            </select>
          </label>
          <label className="field">
            Execution model
            <select value={form.model} onChange={(e) => set("model", e.target.value as "candle" | "quote")}>
              <option value="candle">candle (bars)</option>
              <option value="quote">quote (L1 bid/ask)</option>
            </select>
          </label>
          <label className="field">
            Start (UTC, optional)
            <input type="datetime-local" value={form.start} onChange={(e) => set("start", e.target.value)} />
          </label>
          <label className="field">
            End (UTC, optional)
            <input type="datetime-local" value={form.end} onChange={(e) => set("end", e.target.value)} />
          </label>
        </div>
        <fieldset className="row" style={{ border: 0, padding: 0 }}>
          <legend className="muted" style={{ fontSize: 12 }}>
            Universe
          </legend>
          {symbols.map((s) => (
            <label key={s} className="check">
              <input
                type="checkbox"
                checked={universe.includes(s)}
                onChange={() => setUniverse((u) => (u.includes(s) ? u.filter((x) => x !== s) : [...u, s]))}
              />
              {s}
            </label>
          ))}
          {symbols.length === 0 && <span className="muted">This dataset has no {kind}.</span>}
        </fieldset>
      </section>

      <section className="panel">
        <h2>Strategy</h2>
        <div className="row">
          <label className="field">
            Strategy
            <select value={strategy} onChange={(e) => setStrategy(e.target.value)}>
              {strategies.data.map((s) => (
                <option key={s.name} value={s.name}>
                  {s.name}
                </option>
              ))}
            </select>
          </label>
        </div>
        {info ? <SchemaForm schema={info.params_schema as ParamSchema} values={params} onChange={setParams} /> : <Loading />}
      </section>

      <section className="panel">
        <h2>Capital, costs and risk</h2>
        <div className="row">
          <label className="field">
            Initial capital
            <input value={form.capital} onChange={(e) => set("capital", e.target.value)} required />
          </label>
          {form.model === "candle" && (
            <>
              <label className="field">
                Slippage
                <select value={form.slippageKind} onChange={(e) => set("slippageKind", e.target.value as typeof form.slippageKind)}>
                  <option value="none">none</option>
                  <option value="fixed_bps">fixed bps</option>
                  <option value="half_spread">half spread</option>
                </select>
              </label>
              <label className="field">
                Slippage bps
                <input value={form.slippageBps} onChange={(e) => set("slippageBps", e.target.value)} disabled={form.slippageKind === "none"} />
              </label>
            </>
          )}
          <label className="field">
            Maker fee bps
            <input value={form.makerBps} onChange={(e) => set("makerBps", e.target.value)} />
          </label>
          <label className="field">
            Taker fee bps
            <input value={form.takerBps} onChange={(e) => set("takerBps", e.target.value)} />
          </label>
          <label className="field">
            Min fee
            <input value={form.minFee} onChange={(e) => set("minFee", e.target.value)} />
          </label>
          <label className="field">
            Max leverage
            <input value={form.maxLeverage} onChange={(e) => set("maxLeverage", e.target.value)} />
          </label>
          <label className="field">
            Seed
            <input value={form.seed} onChange={(e) => set("seed", e.target.value)} />
          </label>
        </div>
        <div className="row">
          <label className="check">
            <input type="checkbox" checked={form.allowShort} onChange={(e) => set("allowShort", e.target.checked)} />
            Allow shorts
          </label>
          <label className="check">
            <input type="checkbox" checked={form.benchmark} onChange={(e) => set("benchmark", e.target.checked)} />
            Buy-and-hold benchmark
          </label>
        </div>
      </section>

      <ErrorBox error={error} />
      <button type="submit" disabled={busy || universe.length === 0}>
        Start backtest
      </button>
    </form>
  );
}
