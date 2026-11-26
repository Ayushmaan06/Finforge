import { defineConfig, devices } from "@playwright/test";

// Demo 1 end-to-end (PLAN §13): the backend stack (Postgres, API, worker, one-year demo dataset)
// and the production build of the dashboard are started by `webServer` below.
const API_PORT = 8765;
const WEB_PORT = 3100;
const PY = process.env.FINFORGE_PYTHON ?? "../.venv/bin/python";
// E2E_BASE_URL: test a stack that is already running (docker compose) instead of starting one.
const EXTERNAL = process.env.E2E_BASE_URL;

export default defineConfig({
  testDir: "./e2e",
  timeout: 240_000,
  expect: { timeout: 20_000 },
  fullyParallel: false,
  workers: 1,
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : "list",
  use: {
    baseURL: EXTERNAL ?? `http://127.0.0.1:${WEB_PORT}`,
    trace: "retain-on-failure",
    ...devices["Desktop Chrome"],
  },
  webServer: EXTERNAL ? undefined : [
    {
      command: `${PY} ../scripts/e2e_stack.py --port ${API_PORT}`,
      url: `http://127.0.0.1:${API_PORT}/health`,
      timeout: 180_000,
      reuseExistingServer: !process.env.CI,
      stdout: "pipe",
      // SIGTERM lets the stack stop its worker and its throwaway PostgreSQL instance.
      gracefulShutdown: { signal: "SIGTERM", timeout: 20_000 },
    },
    {
      command: `npm run build && npx next start -p ${WEB_PORT} -H 127.0.0.1`,
      url: `http://127.0.0.1:${WEB_PORT}`,
      timeout: 240_000,
      reuseExistingServer: !process.env.CI,
      env: { NEXT_PUBLIC_API_URL: `http://127.0.0.1:${API_PORT}`, NEXT_TELEMETRY_DISABLED: "1" },
      gracefulShutdown: { signal: "SIGTERM", timeout: 10_000 },
    },
  ],
});
