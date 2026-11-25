"use client";

// Time series charts with TradingView lightweight-charts (canvas). Data arrive already downsampled
// by the API (LTTB), so each series has at most a few thousand points.
import { useEffect, useRef } from "react";

export type Point = { time: number; value: number }; // time: ns since the epoch (UTC)
export type Series = { name: string; color: string; data: Point[]; kind?: "line" | "area" };
export type ChartMarker = { time: number; side: "buy" | "sell"; text?: string };

type Props = {
  series: Series[];
  markers?: { series: number; items: ChartMarker[] };
  height?: number;
  percent?: boolean;
  testId?: string;
  /** Called once the data are on the canvas (after the next animation frame). */
  onRendered?: () => void;
};

/** ns → whole seconds, strictly increasing (later points win on duplicates). */
function toSeconds(data: Point[]) {
  const out: { time: number; value: number }[] = [];
  for (const p of data) {
    const t = Math.floor(p.time / 1e9);
    if (out.length && out[out.length - 1].time >= t) out[out.length - 1] = { time: out[out.length - 1].time, value: p.value };
    else out.push({ time: t, value: p.value });
  }
  return out;
}

let charts: Promise<typeof import("lightweight-charts")> | null = null;

/** Start loading the chart library early (e.g. while the data are being fetched). */
export function preloadCharts() {
  charts ??= import("lightweight-charts");
  return charts;
}

export default function TimeChart({ series, markers, height = 300, percent, testId, onRendered }: Props) {
  const ref = useRef<HTMLDivElement>(null);
  const rendered = useRef(onRendered);
  useEffect(() => {
    rendered.current = onRendered;
  });

  useEffect(() => {
    let disposed = false;
    let cleanup = () => {};
    (async () => {
      const lc = await preloadCharts();
      if (disposed || !ref.current) return;
      const el = ref.current;
      const css = getComputedStyle(document.documentElement);
      const chart = lc.createChart(el, {
        height,
        autoSize: true,
        layout: { background: { color: "transparent" }, textColor: css.getPropertyValue("--muted").trim() || "#667085" },
        grid: { vertLines: { visible: false }, horzLines: { color: css.getPropertyValue("--line").trim() || "#e3e6ec" } },
        timeScale: { timeVisible: true, secondsVisible: false },
        localization: percent ? { priceFormatter: (v: number) => `${(v * 100).toFixed(1)}%` } : undefined,
        crosshair: { mode: lc.CrosshairMode.Normal },
      });
      const handles = series.map((s) => {
        const h =
          s.kind === "area"
            ? chart.addSeries(lc.AreaSeries, { lineColor: s.color, topColor: `${s.color}55`, bottomColor: `${s.color}05`, lineWidth: 1, title: s.name })
            : chart.addSeries(lc.LineSeries, { color: s.color, lineWidth: 2, title: s.name, priceLineVisible: false });
        h.setData(toSeconds(s.data) as Parameters<typeof h.setData>[0]);
        return h;
      });
      if (markers && handles[markers.series]) {
        const items = [...markers.items]
          .sort((a, b) => a.time - b.time)
          .map((m) => ({
            time: Math.floor(m.time / 1e9),
            position: m.side === "buy" ? ("belowBar" as const) : ("aboveBar" as const),
            shape: m.side === "buy" ? ("arrowUp" as const) : ("arrowDown" as const),
            color: m.side === "buy" ? "#16a34a" : "#dc2626",
            text: m.text,
          }));
        const target = handles[markers.series] as unknown as Parameters<typeof lc.createSeriesMarkers>[0];
        lc.createSeriesMarkers(target, items as Parameters<typeof lc.createSeriesMarkers>[1]);
      }
      chart.timeScale().fitContent();
      requestAnimationFrame(() => {
        if (!disposed) {
          el.dataset.rendered = "true";
          rendered.current?.();
        }
      });
      cleanup = () => chart.remove();
    })();
    return () => {
      disposed = true;
      cleanup();
    };
  }, [series, markers, height, percent]);

  return (
    <div>
      <div className="legend">
        {series.map((s) => (
          <span key={s.name} style={{ ["--c" as string]: s.color }}>
            {s.name}
          </span>
        ))}
      </div>
      <div ref={ref} className="chart" style={{ height }} data-testid={testId} />
    </div>
  );
}
