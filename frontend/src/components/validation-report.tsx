import { int } from "@/lib/format";

export default function ValidationReport({ report }: { report: Record<string, unknown> }) {
  const findings = (report.findings as Record<string, unknown>[] | undefined) ?? [];
  return (
    <div aria-label="Validation report">
      <p>
        Validation: <strong>{String(report.status)}</strong> — rows read {int(report.rows_read as number)}, written{" "}
        {int(report.rows_written as number)}, dropped {int(report.rows_dropped as number)}
      </p>
      {findings.length > 0 && (
        <table>
          <thead>
            <tr>
              <th>Action</th>
              <th>Code</th>
              <th>Source</th>
              <th className="num">Count</th>
            </tr>
          </thead>
          <tbody>
            {findings.map((f, i) => (
              <tr key={i}>
                <td>{String(f.action)}</td>
                <td>{String(f.code)}</td>
                <td>
                  {String(f.kind)}/{String(f.symbol)}
                </td>
                <td className="num">{int(f.count as number)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  );
}
