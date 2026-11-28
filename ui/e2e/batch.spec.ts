import { expect, test } from "./editor";

test("removes sample-accurate edits from the Batch menu and undoes it", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy("protools/multichannel_wav.aaf"));
  const rendered = () =>
    page.evaluate(() => (window.__aafTimeline?.tracks() ?? []).flatMap((t) => t.items.filter((i) => i.rendered === "seam" || i.rendered === "region")).length);
  await expect.poll(rendered).toBe(17);
  await page.getByRole("button", { name: "Batch" }).click();
  await page.getByRole("menuitem", { name: "Remove sample-accurate edits" }).click();
  await expect(page.locator(".message.info")).toContainText("Removed 17 sample-accurate edits (10 cuts, 7 silenced); deleted 180 unused mobs.");
  await expect.poll(rendered).toBe(0);
  await expect(page.getByRole("menuitem")).toHaveCount(0);
  await page.getByRole("button", { name: "Undo" }).click();
  await expect.poll(rendered).toBe(17);
});
