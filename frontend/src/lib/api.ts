// Typed client for the FinForge API. Types are generated from the API's OpenAPI document
// (`npm run gen:api` → src/lib/api-schema.d.ts); CI fails if they drift.
import createClient from "openapi-fetch";

import type { components, paths } from "./api-schema";

export type Schemas = components["schemas"];
export type Run = Schemas["RunOut"];
export type Dataset = Schemas["DatasetOut"];
export type DatasetBrief = Schemas["DatasetBrief"];
export type Strategy = Schemas["StrategyOut"];
export type StrategyBrief = Schemas["StrategyBrief"];
export type Equity = Schemas["EquityOut"];
export type Prices = Schemas["PricesOut"];
export type Returns = Schemas["ReturnsOut"];
export type Compare = Schemas["CompareOut"];
export type RunRequest = Schemas["RunIn"];
export type DatasetRequest = Schemas["DatasetIn"];

export const API_URL = (process.env.NEXT_PUBLIC_API_URL ?? "http://127.0.0.1:8000").replace(/\/$/, "");

export const api = createClient<paths>({ baseUrl: API_URL });

export class ApiError extends Error {
  constructor(
    public status: number,
    public detail: unknown,
  ) {
    super(describe(detail) || `HTTP ${status}`);
  }
}

function describe(detail: unknown): string {
  if (typeof detail === "string") return detail;
  if (Array.isArray(detail)) {
    return detail
      .map((d) => (typeof d === "object" && d && "msg" in d ? `${(d as { loc?: unknown[] }).loc?.slice(1).join(".") ?? ""}: ${(d as { msg: string }).msg}` : JSON.stringify(d)))
      .join("; ");
  }
  if (detail && typeof detail === "object" && "message" in detail) return String((detail as { message: unknown }).message);
  return detail === undefined ? "" : JSON.stringify(detail);
}

/** Unwrap an openapi-fetch result, throwing ApiError with the server's `detail`. */
export function ok<T>(r: { data?: T; error?: unknown; response: Response }): T {
  if (r.data === undefined || r.error !== undefined) {
    const detail = (r.error as { detail?: unknown } | undefined)?.detail ?? r.error;
    throw new ApiError(r.response.status, detail);
  }
  return r.data;
}

/** Multipart upload (openapi-fetch serialises JSON bodies only). */
export async function uploadFile(file: File): Promise<Schemas["UploadOut"]> {
  const form = new FormData();
  form.append("file", file);
  const res = await fetch(`${API_URL}/uploads`, { method: "POST", body: form });
  const body = await res.json();
  if (!res.ok) throw new ApiError(res.status, body.detail);
  return body;
}

export function eventsUrl(runId: string): string {
  return `${API_URL}/runs/${runId}/events?poll=0.25`;
}
