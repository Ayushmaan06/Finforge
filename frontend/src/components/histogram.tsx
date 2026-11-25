// Returns histogram as plain SVG: a few dozen bars do not need a charting library.
type Props = { values: number[]; compare?: number[]; bins?: number; label: string };

export default function Histogram({ values, compare, bins = 30, label }: Props) {
  const all = compare ? values.concat(compare) : values;
  if (all.length === 0) return <p className="muted">No returns.</p>;
  let lo = Math.min(...all);
  let hi = Math.max(...all);
  if (lo === hi) {
    lo -= 0.001;
    hi += 0.001;
  }
  const width = (hi - lo) / bins;
  const count = (xs: number[]) => {
    const c = new Array(bins).fill(0);
    for (const x of xs) c[Math.min(bins - 1, Math.floor((x - lo) / width))]++;
    return c;
  };
  const a = count(values);
  const b = compare ? count(compare) : null;
  const max = Math.max(...a, ...(b ?? [0]));
  const W = 600;
  const H = 180;
  const bw = W / bins;
  const zero = ((0 - lo) / (hi - lo)) * W;
  return (
    <figure style={{ margin: 0 }}>
      <svg viewBox={`0 0 ${W} ${H + 20}`} width="100%" role="img" aria-label={label}>
        {a.map((n, i) => (
          <rect key={`a${i}`} x={i * bw + 1} y={H - (n / max) * H} width={b ? bw / 2 - 1 : bw - 2} height={(n / max) * H} fill="var(--accent)" />
        ))}
        {b?.map((n, i) => (
          <rect key={`b${i}`} x={i * bw + bw / 2} y={H - (n / max) * H} width={bw / 2 - 1} height={(n / max) * H} fill="var(--muted)" opacity={0.6} />
        ))}
        {zero >= 0 && zero <= W && <line x1={zero} x2={zero} y1={0} y2={H} stroke="var(--text)" strokeDasharray="3 3" />}
        <text x={0} y={H + 16} fontSize="11" fill="var(--muted)">{`${(lo * 100).toFixed(2)}%`}</text>
        <text x={W} y={H + 16} fontSize="11" fill="var(--muted)" textAnchor="end">{`${(hi * 100).toFixed(2)}%`}</text>
      </svg>
      <figcaption className="muted" style={{ fontSize: 12 }}>
        {label}
        {b ? " (blue: strategy, grey: benchmark)" : ""}
      </figcaption>
    </figure>
  );
}
