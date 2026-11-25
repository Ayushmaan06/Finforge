# FinForge dashboard

Next.js (App Router) + TypeScript research dashboard for the FinForge API. It runs Demo 1 in the
browser: pick a dataset, configure and start a backtest, watch progress, inspect results, compare
runs. See [docs/dashboard.md](../docs/dashboard.md).

```bash
npm ci
NEXT_PUBLIC_API_URL=http://127.0.0.1:8000 npm run dev   # API: finforge server api (CORS allows :3000)
npm run gen:api      # regenerate src/lib/api-schema.d.ts from the API's OpenAPI document
npm run lint && npm run typecheck
npx playwright test  # Demo 1 end to end; starts the whole stack itself
```
