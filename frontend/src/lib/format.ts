export function pct(v: number | null | undefined, digits = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "–";
  return `${(v * 100).toFixed(digits)}%`;
}

export function signedPct(v: number | null | undefined, digits = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "–";
  return `${v >= 0 ? "+" : ""}${(v * 100).toFixed(digits)}%`;
}

export function num(v: number | null | undefined, digits = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "–";
  return v.toLocaleString("en-US", { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

export function int(v: number | null | undefined): string {
  if (v === null || v === undefined) return "–";
  return v.toLocaleString("en-US");
}

/** ns since the epoch (UTC) → "2024-01-31 23:59 UTC". */
export function nsDate(ns: number | null | undefined, withTime = true): string {
  if (ns === null || ns === undefined) return "–";
  const iso = new Date(ns / 1e6).toISOString();
  return withTime ? `${iso.slice(0, 16).replace("T", " ")} UTC` : iso.slice(0, 10);
}

export function isoDate(s: string | null | undefined): string {
  if (!s) return "–";
  return `${s.slice(0, 19).replace("T", " ")} UTC`;
}

export function duration(seconds: number | null | undefined): string {
  if (seconds === null || seconds === undefined || !Number.isFinite(seconds)) return "–";
  if (seconds < 120) return `${seconds.toFixed(0)} s`;
  if (seconds < 7200) return `${(seconds / 60).toFixed(1)} min`;
  if (seconds < 172800) return `${(seconds / 3600).toFixed(1)} h`;
  return `${(seconds / 86400).toFixed(1)} d`;
}

export function shortId(id: string): string {
  return id.slice(0, 8);
}

/** A drawdown fraction (positive) shown as a loss: 0.12 → "−12.00%". */
export function dd(v: number | null | undefined, digits = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "–";
  return v === 0 ? "0.00%" : `−${(v * 100).toFixed(digits)}%`;
}
