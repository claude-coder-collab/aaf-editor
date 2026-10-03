import { expect, test } from "./editor";

const SAMPLE = "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf";
const OTHER = "protools/multichannel_frame_aligned.aaf";

test("shows a hint while files are dragged over the window", async ({ editor }) => {
  const { page } = editor;
  // Keep dragging (as a real drag does, with a dragover every few tens of milliseconds) while checking.
  await page.evaluate(() => {
    const data = new DataTransfer();
    data.items.add(new File(["x"], "edit.aaf"));
    const timer = setInterval(() => window.dispatchEvent(new DragEvent("dragover", { dataTransfer: data, bubbles: true })), 50);
    (window as { stopDrag?: () => void }).stopDrag = () => clearInterval(timer);
  });
  await expect(page.locator(".drop-hint")).toContainText("Drop an AAF file to open it");
  await page.evaluate(() => (window as { stopDrag?: () => void }).stopDrag?.());
  await expect(page.locator(".drop-hint")).toHaveCount(0, { timeout: 3000 });
});

test("opens a dropped file, asking before discarding unsaved changes", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  const dropped = editor.copy(OTHER);

  await page.locator("#search").fill("LUCID");
  await page.locator("#search").press("Enter");
  await page.locator(".results .result").filter({ hasText: "CompositionMob" }).first().click();
  const name = page.locator(".panel .row").filter({ has: page.locator(".name", { hasText: /^\s*Name\s*\*?\s*$/ }) }).locator("input");
  await name.fill("Edited");
  await name.press("Enter");
  await expect(page.locator(".toolbar .dirty")).toBeVisible();

  page.once("dialog", (dialog) => {
    expect(dialog.message()).toContain("unsaved changes");
    void dialog.dismiss();
  });
  await page.evaluate((path) => window.__aafOpenFile!(path), dropped);
  await expect(page.locator(".toolbar .file")).toContainText("RealWorldSample1.aaf");

  page.once("dialog", (dialog) => void dialog.accept());
  await page.evaluate((path) => window.__aafOpenFile!(path), dropped);
  await expect(page.locator(".toolbar .file")).toContainText("multichannel_frame_aligned.aaf");
  await expect(page.locator(".toolbar .dirty")).toHaveCount(0);
});
