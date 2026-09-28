import { readFileSync } from "node:fs";

import { expect, test } from "./editor";

const file = process.env.AAF_PERF_FILE;

test.skip(!file, "Set AAF_PERF_FILE to an AAF file to measure the timeline's performance");

test("timeline performance", async ({ editor }, testInfo) => {
  test.setTimeout(300_000);
  const { page } = editor;
  const results: Record<string, number> = {};
  const clipCount = () => page.evaluate(() => (window.__aafTimeline?.tracks() ?? []).reduce((n, t) => n + t.items.length, 0));

  let start = Date.now();
  editor.host.openPath = file!;
  await page.getByRole("button", { name: "Open…" }).first().click();
  await expect.poll(clipCount, { timeout: 120_000, intervals: [50] }).toBeGreaterThan(0);
  results["open to timeline shown (ms)"] = Date.now() - start;
  results["items"] = await clipCount();

  const fit = await page.evaluate(() => window.__aafTimeline!.measure(20));
  results["draw at fit (ms)"] = fit.draw;
  results["hit test at fit (ms)"] = fit.pick;
  await page.evaluate(() => window.__aafTimeline!.zoom(200));
  const zoomed = await page.evaluate(() => window.__aafTimeline!.measure(20));
  results["draw zoomed in (ms)"] = zoomed.draw;
  results["hit test zoomed in (ms)"] = zoomed.pick;
  await page.evaluate(() => window.__aafTimeline!.zoom(1 / 200));

  const canvas = (await page.locator(".timeline canvas").boundingBox())!;
  start = Date.now();
  for (let n = 0; n < 20; n++) await page.mouse.move(canvas.x + 200 + n * 10, canvas.y + 60);
  results["mouse move round trip (ms)"] = (Date.now() - start) / 20;

  const tracksBefore = await page.evaluate(() => window.__aafTimeline!.tracks().length);
  results["projected timeline JSON in page (bytes)"] = await page.evaluate(() => JSON.stringify(window.__aafTimeline!.tracks()).length);
  start = Date.now();
  await page.getByRole("button", { name: "+A" }).click();
  await expect.poll(() => page.evaluate(() => window.__aafTimeline!.tracks().length), { timeout: 60_000, intervals: [20] }).toBe(tracksBefore + 1);
  results["edit to redraw (add track) (ms)"] = Date.now() - start;

  const heap = await page.evaluate(() => (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize ?? 0);
  if (heap) results["JS heap (MB)"] = heap / 1024 / 1024;

  const lines = Object.entries(results).map(([k, v]) => `${k.padEnd(44)} ${Number.isInteger(v) ? v : v.toFixed(2)}`);
  console.log(`\n${testInfo.project.name}\n${lines.join("\n")}`);
  await testInfo.attach("results", { body: JSON.stringify(results, null, 2), contentType: "application/json" });

  const limitsFile = process.env.AAF_PERF_LIMITS;
  if (limitsFile) {
    const limits = (JSON.parse(readFileSync(limitsFile, "utf8")) as { ui: Record<string, number> }).ui;
    for (const [name, limit] of Object.entries(limits)) {
      expect.soft(results[name], name).toBeLessThanOrEqual(limit);
    }
  }
});
