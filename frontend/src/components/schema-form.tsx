"use client";

// A form generated from a strategy's parameter JSON Schema (the same schema the API validates).
type Prop = { type?: string | string[]; anyOf?: Prop[]; description?: string; minimum?: number };
export type ParamSchema = { properties?: Record<string, Prop>; required?: string[] };

function kind(p: Prop): "integer" | "number" | "boolean" | "text" {
  if (p.anyOf) return "text"; // e.g. quantities: decimal string or number
  const t = Array.isArray(p.type) ? p.type[0] : p.type;
  return t === "integer" || t === "number" || t === "boolean" ? t : "text";
}

/** Convert raw input values to the types the schema expects. Empty optional values are omitted. */
export function coerceParams(schema: ParamSchema, raw: Record<string, string | boolean>) {
  const out: Record<string, unknown> = {};
  for (const [name, p] of Object.entries(schema.properties ?? {})) {
    const v = raw[name];
    if (v === undefined || v === "") continue;
    const k = kind(p);
    out[name] = k === "integer" ? parseInt(String(v), 10) : k === "number" ? Number(v) : k === "boolean" ? Boolean(v) : String(v);
  }
  return out;
}

export default function SchemaForm({
  schema,
  values,
  onChange,
}: {
  schema: ParamSchema;
  values: Record<string, string | boolean>;
  onChange: (v: Record<string, string | boolean>) => void;
}) {
  const props = Object.entries(schema.properties ?? {});
  if (props.length === 0) return <p className="muted">This strategy has no parameters.</p>;
  return (
    <div className="row">
      {props.map(([name, p]) => {
        const k = kind(p);
        const required = schema.required?.includes(name);
        if (k === "boolean") {
          return (
            <label key={name} className="check">
              <input type="checkbox" checked={Boolean(values[name])} onChange={(e) => onChange({ ...values, [name]: e.target.checked })} />
              {name}
            </label>
          );
        }
        return (
          <label key={name} className="field">
            {name}
            {required ? " *" : ""}
            <input
              name={name}
              type={k === "text" ? "text" : "number"}
              step={k === "integer" ? 1 : "any"}
              required={required}
              value={String(values[name] ?? "")}
              title={p.description}
              onChange={(e) => onChange({ ...values, [name]: e.target.value })}
            />
          </label>
        );
      })}
    </div>
  );
}
