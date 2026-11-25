"use client";

import Link from "next/link";

import { ErrorBox, Loading } from "@/components/ui";
import { api, ok } from "@/lib/api";
import { useApi } from "@/lib/use-api";

export default function StrategiesPage() {
  const list = useApi(async () => ok(await api.GET("/strategies")));
  return (
    <>
      <h1>Strategies</h1>
      <p className="muted">Built-in strategies only: the API never runs uploaded code.</p>
      <ErrorBox error={list.error} />
      {!list.data ? (
        <Loading />
      ) : (
        <section className="panel">
          <table>
            <thead>
              <tr>
                <th>Name</th>
                <th>Summary</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {list.data.map((s) => (
                <tr key={s.name}>
                  <td>
                    <Link href={`/strategies/${s.name}`}>{s.name}</Link>
                  </td>
                  <td>{s.summary}</td>
                  <td>
                    <Link href={`/runs/new?strategy=${s.name}`}>Backtest</Link>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      )}
    </>
  );
}
