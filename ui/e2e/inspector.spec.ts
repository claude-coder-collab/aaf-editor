import { aaftool, expect, test } from "./editor";

const SAMPLE = "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf";

test("opens a file and shows the header", async ({ editor }) => {
  const { page, host } = editor;
  await editor.open(editor.copy(SAMPLE));
  await expect(page.locator(".panel h2")).toHaveText("Header");
  await expect(page.getByRole("treeitem").first()).toBeVisible();
  expect(host.titles.at(-1)).toBe("RealWorldSample1.aaf — AAF Editor");
});

test("edits a mob name, undoes and redoes it", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  await page.locator("#search").fill("LUCID");
  await page.locator("#search").press("Enter");
  await page.locator(".results .result").filter({ hasText: "CompositionMob" }).first().click();
  await expect(page.locator(".panel .subtitle")).toContainText("CompositionMob");

  const name = page.locator(".panel .row").filter({ has: page.locator(".name", { hasText: /^\s*Name\s*\*?\s*$/ }) }).locator("input");
  await expect(name).toHaveValue("LUCID(a)");
  await name.fill("Renamed in e2e");
  await name.press("Enter");

  await expect(page.locator(".panel h2")).toHaveText("Renamed in e2e");
  await expect(page.locator(".toolbar .dirty")).toBeVisible();
  await expect(page.getByRole("button", { name: /^History/ })).toContainText("(1)");
  await expect(page.locator(".timeline .controls strong")).toHaveText("Renamed in e2e");

  await page.locator(".panel h2").click();
  await page.keyboard.press("ControlOrMeta+z");
  await expect(page.locator(".panel h2")).toHaveText("LUCID(a)");
  await expect(page.locator(".toolbar .dirty")).toHaveCount(0);
  await page.getByRole("button", { name: "Redo" }).click();
  await expect(page.locator(".panel h2")).toHaveText("Renamed in e2e");
});

test("rejects an invalid integer inline", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  const version = page.locator(".panel .row").filter({ has: page.locator(".name", { hasText: /^\s*ObjectModelVersion/ }) }).locator("input");
  await version.fill("not a number");
  await version.press("Enter");
  await expect(page.locator(".panel .scalar .error")).toBeVisible();
  await expect(page.getByRole("button", { name: /^History/ })).toContainText("(0)");
});

test("validates the file", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  await page.getByRole("button", { name: "Validate" }).click();
  await expect(page.getByRole("button", { name: /^Diagnostics/ })).toContainText("0 errors");
});

test("saves an edited copy that aaftool validates", async ({ editor }, testInfo) => {
  const { page, host } = editor;
  await editor.open(editor.copy(SAMPLE));
  await page.locator("#search").fill("LUCID");
  await page.locator("#search").press("Enter");
  await page.locator(".results .result").filter({ hasText: "CompositionMob" }).first().click();
  const name = page.locator(".panel .row").filter({ has: page.locator(".name", { hasText: /^\s*Name\s*\*?\s*$/ }) }).locator("input");
  await name.fill("Saved by e2e");
  await name.press("Enter");
  await expect(page.locator(".toolbar .dirty")).toBeVisible();

  host.savePath = testInfo.outputPath("saved.aaf");
  await page.getByRole("button", { name: "Save As…" }).click();
  await expect(page.locator(".toolbar .file")).toContainText("saved.aaf");
  await expect(page.locator(".toolbar .dirty")).toHaveCount(0);

  expect(aaftool("validate", host.savePath).status).toBe(0);
  expect(aaftool("timeline", host.savePath, "--mobs").stdout).toContain("Saved by e2e");
});
