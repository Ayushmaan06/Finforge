import path from "node:path";

import { expect, test, type Page } from "@playwright/test";

// Demo 1: pick a dataset → MA crossover → parameters, capital, fees, slippage, model → start →
// watch progress → equity vs benchmark, trades, drawdown, risk → compare with a second parameter set.
//
// Next.js keeps recently visited routes mounted but hidden (React Activity), so locators use roles
// or filter on visibility.

const API = process.env.E2E_API_URL ?? "http://127.0.0.1:8765"; // docker compose: :8000
const FIXTURES = path.resolve(__dirname, "../../datasets/fixtures");

async function startBacktest(page: Page, datasetId: string, fast: string, slow: string): Promise<string> {
  await page.goto(`/runs/new?dataset=${datasetId}&strategy=ma_cross`);
  await expect(page.getByRole("heading", { name: "New backtest" })).toBeVisible();
  await page.getByRole("combobox", { name: /^Execution model/ }).selectOption("candle");
  await page.getByRole("spinbutton", { name: "fast" }).fill(fast);
  await page.getByRole("spinbutton", { name: "slow" }).fill(slow);
  await page.getByRole("textbox", { name: "qty" }).fill("10");
  await page.getByRole("textbox", { name: "Initial capital" }).fill("100000");
  await page.getByRole("combobox", { name: /^Slippage/ }).selectOption("fixed_bps");
  await page.getByRole("textbox", { name: "Slippage bps" }).fill("1");
  await page.getByRole("textbox", { name: "Taker fee bps" }).fill("10");
  await page.getByRole("button", { name: "Start backtest" }).click();
  await page.waitForURL(/\/runs\/[0-9a-f-]{36}$/);
  return page.url().split("/").pop()!;
}

test("Demo 1 in the browser", async ({ page, request }) => {
  // --- Datasets: upload a file, validate, create ---------------------------------------------
  await page.goto("/datasets");
  await page.getByRole("textbox", { name: "Dataset name" }).fill("fixture AAA");
  await page.getByRole("textbox", { name: "Symbol" }).fill("AAA");
  await page.getByLabel("File").filter({ visible: true }).setInputFiles(path.join(FIXTURES, "klines_AAA_ms.csv"));
  await page.getByRole("button", { name: "Validate" }).click();
  await expect(page.getByLabel("Validation report").filter({ visible: true })).toContainText("clean");
  await page.getByRole("button", { name: "Create dataset" }).click();
  await expect(page.getByRole("link", { name: "Backtest on it →" })).toBeVisible();

  // --- Pick the one-year demo dataset ----------------------------------------------------------
  const datasets = await (await request.get(`${API}/datasets`)).json();
  const demo = datasets.find((d: { name: string }) => d.name.startsWith("demo-"));
  expect(demo, "the e2e stack registers a demo dataset").toBeTruthy();

  // --- Strategy documentation is browsable -----------------------------------------------------
  await page.goto("/strategies/ma_cross");
  await expect(page.getByRole("heading", { name: "Parameters" })).toBeVisible();

  // --- First run: watch progress, then inspect results -----------------------------------------
  const first = await startBacktest(page, demo.id, "60", "720");
  await expect(page.getByRole("heading", { name: new RegExp(`Run ${first.slice(0, 8)}`) })).toBeVisible();
  await expect(page.getByRole("progressbar")).toBeVisible(); // live progress (SSE)
  await expect(page.getByText("completed", { exact: true }).filter({ visible: true })).toBeVisible({ timeout: 180_000 });

  // Equity vs benchmark: drawn from 525 600 points downsampled on the server, in under 1 s.
  const render = page.getByTestId("equity-render").filter({ visible: true });
  await expect(render).toHaveAttribute("data-render-ms", /\d/);
  const ms = Number(await render.getAttribute("data-render-ms"));
  console.log(`equity chart: fetched + drawn in ${ms.toFixed(0)} ms`);
  expect(ms).toBeLessThan(1000);
  await expect(render).toContainText("of 525,600 points");
  await expect(page.getByText("Buy and hold", { exact: true }).filter({ visible: true })).toBeVisible();

  for (const id of ["equity-chart", "drawdown-chart", "price-chart"]) {
    await expect(page.getByTestId(id).filter({ visible: true })).toHaveAttribute("data-rendered", "true");
  }
  await expect(page.getByRole("img", { name: "Distribution of daily returns" })).toBeVisible();
  const metrics = page.getByLabel("Metrics").filter({ visible: true });
  for (const label of ["Total return", "Benchmark return", "Sharpe", "Max drawdown", "VaR 95% (1d)", "Trades"]) {
    await expect(metrics).toContainText(label);
  }
  const trades = page.getByRole("region", { name: "Trades" });
  await expect(trades.getByRole("row").nth(1)).toBeVisible(); // at least one round trip
  await expect(page.getByRole("heading", { name: "Execution" })).toBeVisible();

  // --- Second parameter set, then compare -----------------------------------------------------
  const second = await startBacktest(page, demo.id, "120", "1440");
  await expect(page.getByText("completed", { exact: true }).filter({ visible: true })).toBeVisible({ timeout: 180_000 });

  await page.goto(`/compare?runs=${first},${second}`);
  const table = page.getByRole("region", { name: "Comparison table" });
  await expect(table.getByRole("row")).toHaveCount(3); // header + 2 runs
  await expect(page.getByTestId("compare-equity").filter({ visible: true })).toHaveAttribute("data-rendered", "true");
  await page.getByLabel("In-sample / out-of-sample split (UTC)").filter({ visible: true }).fill("2024-07-01T00:00");
  await page.getByRole("button", { name: "Apply split" }).click();
  await expect(table.getByRole("columnheader", { name: "OOS Sharpe" })).toBeVisible();
});
