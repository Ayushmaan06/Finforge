import Link from "next/link";

export function Status({ status }: { status: string }) {
  return <span className={`badge ${status}`}>{status}</span>;
}

export function Card({ label, value, tone }: { label: string; value: string; tone?: "good" | "bad" }) {
  return (
    <div className="card">
      <div className="label">{label}</div>
      <div className={`value ${tone ?? ""}`}>{value}</div>
    </div>
  );
}

export function ErrorBox({ error }: { error?: Error | string }) {
  if (!error) return null;
  return (
    <div className="error" role="alert">
      {typeof error === "string" ? error : error.message}
    </div>
  );
}

export function Loading({ what = "Loading" }: { what?: string }) {
  return <p className="muted">{what}…</p>;
}

export function RunLink({ id }: { id: string }) {
  return <Link href={`/runs/${id}`}>{id.slice(0, 8)}</Link>;
}

export function tone(v: number | null | undefined): "good" | "bad" | undefined {
  if (v === null || v === undefined || v === 0) return undefined;
  return v > 0 ? "good" : "bad";
}
