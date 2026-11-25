"use client";

import Link from "next/link";
import { use } from "react";

import { ErrorBox, Loading } from "@/components/ui";
import { api, ok } from "@/lib/api";
import { useApi } from "@/lib/use-api";

export default function StrategyView({ params }: { params: Promise<{ name: string }> }) {
  const { name } = use(params);
  const s = useApi(async () => ok(await api.GET("/strategies/{name}", { params: { path: { name } } })), name);
  if (s.error) return <ErrorBox error={s.error} />;
  if (!s.data) return <Loading />;
  const props = Object.entries((s.data.params_schema.properties ?? {}) as Record<string, Record<string, unknown>>);
  const required = (s.data.params_schema.required ?? []) as string[];
  return (
    <>
      <h1>{s.data.name}</h1>
      <p>
        <Link href={`/runs/new?strategy=${s.data.name}`}>
          <button>Backtest with {s.data.name}</button>
        </Link>
      </p>
      <section className="panel">
        <h2>Documentation</h2>
        <pre>{s.data.description}</pre>
      </section>
      <section className="panel">
        <h2>Parameters</h2>
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Type</th>
              <th>Default</th>
              <th>Constraints</th>
            </tr>
          </thead>
          <tbody>
            {props.map(([k, p]) => (
              <tr key={k}>
                <td>
                  {k}
                  {required.includes(k) ? " *" : ""}
                </td>
                <td>{String(p.type ?? (p.anyOf ? "decimal string or number" : "any"))}</td>
                <td>{JSON.stringify(s.data!.defaults[k] ?? null)}</td>
                <td className="muted">
                  {Object.entries(p)
                    .filter(([c]) => !["type", "anyOf", "description"].includes(c))
                    .map(([c, v]) => `${c} ${JSON.stringify(v)}`)
                    .join(", ")}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <p className="muted" style={{ fontSize: 12 }}>
          Source SHA-256 {s.data.source_sha256}
        </p>
      </section>
    </>
  );
}
