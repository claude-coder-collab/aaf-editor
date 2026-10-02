import { expect, test, type Editor } from "./editor";

const SAMPLE = "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf";

async function tracks(editor: Editor) {
  return editor.page.evaluate(() => window.__aafTimeline?.tracks() ?? []);
}

async function clipBox(editor: Editor, object: number) {
  const canvas = await editor.page.locator(".timeline canvas").boundingBox();
  const rect = await editor.page.evaluate((id) => window.__aafTimeline?.itemRect(id) ?? null, object);
  expect(canvas).not.toBeNull();
  expect(rect).not.toBeNull();
  return { x: canvas!.x + rect!.x, y: canvas!.y + rect!.y, width: rect!.width, height: rect!.height };
}

async function firstClip(editor: Editor) {
  await expect.poll(async () => (await tracks(editor)).length).toBeGreaterThan(0);
  const track = (await tracks(editor))[0]!;
  const clip = track.items.find((i) => i.kind === "sourceClip")!;
  return { track, clip };
}

test("selects a clip on the canvas", async ({ editor }) => {
  await editor.open(editor.copy(SAMPLE));
  const { clip } = await firstClip(editor);
  const box = await clipBox(editor, clip.object);
  await editor.page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await expect(editor.page.locator(".panel .subtitle")).toContainText(`SourceClip · object ${clip.object}`);
  await expect(editor.page.locator(".timeline .chain")).toBeVisible();
});

test("lifts the selected clip and undoes it", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  const { clip } = await firstClip(editor);
  const box = await clipBox(editor, clip.object);
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await page.getByRole("button", { name: "Lift" }).click();
  await expect.poll(async () => (await tracks(editor))[0]!.items.map((i) => i.kind)).toEqual(["filler"]);
  await page.getByRole("button", { name: "Undo" }).click();
  await expect.poll(async () => (await tracks(editor))[0]!.items.map((i) => i.kind)).toEqual(["sourceClip"]);
});

test("splits at the playhead", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  const { clip } = await firstClip(editor);
  const box = await clipBox(editor, clip.object);
  const canvas = (await page.locator(".timeline canvas").boundingBox())!;
  await page.mouse.click(box.x + box.width / 2, canvas.y + 5);
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await page.keyboard.press("s");
  await expect.poll(async () => (await tracks(editor))[0]!.items.filter((i) => i.kind === "sourceClip").length).toBe(2);
  const [a, b] = (await tracks(editor))[0]!.items;
  expect(a!.length + b!.length).toBe(clip.length);
  expect(Math.abs(a!.length - clip.length / 2)).toBeLessThan(clip.length / 20);
});

test("trims the tail by dragging the edge", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  const { clip } = await firstClip(editor);
  const box = await clipBox(editor, clip.object);
  const y = box.y + box.height / 2;
  const edge = box.x + box.width - 2;
  await page.mouse.move(edge, y);
  await page.mouse.down();
  await page.mouse.move(edge - box.width / 4, y, { steps: 5 });
  await page.mouse.up();
  await expect.poll(async () => (await tracks(editor))[0]!.items[0]!.length).toBeLessThan(clip.length);
  const items = (await tracks(editor))[0]!.items;
  expect(items.map((i) => i.kind)).toEqual(["sourceClip"]);
  expect(Math.abs(items[0]!.length - (clip.length * 3) / 4)).toBeLessThan(clip.length / 20);
  await expect(page.getByRole("button", { name: /^History/ })).toContainText("(1)");
});

test("adds a sound track", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  await firstClip(editor);
  const before = (await tracks(editor)).length;
  await page.getByRole("button", { name: "+A" }).click();
  await expect.poll(async () => (await tracks(editor)).length).toBe(before + 1);
  expect((await tracks(editor)).at(-1)!.kind).toBe("sound");
});

test("shows a clip inside effects as the clip, with its effects on a badge", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy(SAMPLE));
  await page.locator("select.mobs").selectOption({ label: "PLAYOUT 151630146.Copy.01 — composition" });
  await expect(page.locator(".timeline .controls strong")).toHaveText("PLAYOUT 151630146.Copy.01");
  await expect.poll(async () => (await tracks(editor)).some((t) => t.items.some((i) => i.effects?.includes("Audio Gain")))).toBe(true);
  const wrapped = (await tracks(editor))
    .flatMap((t) => t.items.filter((i, n) => i.effects?.[0] === "Audio Gain" && i.clip !== undefined && t.items[n - 1]?.kind !== "transition" && t.items[n + 1]?.kind !== "transition"))[0]!;
  await page.evaluate((id) => window.__aafTimeline!.show(id, 300), wrapped.object);
  const canvas = (await page.locator(".timeline canvas").boundingBox())!;
  const box = await clipBox(editor, wrapped.object);
  expect(box.x).toBeGreaterThanOrEqual(canvas.x + 150);
  expect(box.y + box.height).toBeLessThanOrEqual(canvas.y + canvas.height);

  await page.mouse.click(box.x + box.width * 0.75, box.y + box.height / 2);
  await expect(page.locator(".panel .subtitle")).toContainText(`SourceClip · object ${wrapped.clip}`);
  await expect(page.locator(".timeline .chain")).toBeVisible();

  await page.mouse.click(box.x + 14, box.y + box.height / 2);
  await expect(page.locator(".panel .subtitle")).toContainText(`OperationGroup · object ${wrapped.object}`);
  await expect(page.locator(".panel h2")).toHaveText("Audio Gain");
});

test("shows multichannel clips as one clip with their format", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy("protools/multichannel_frame_aligned.aaf"));
  await expect.poll(async () => (await tracks(editor)).filter((t) => t.kind === "sound").map((t) => t.channels ?? 0)).toEqual([0, 2, 6, 8]);
  const surround = (await tracks(editor)).find((t) => t.channels === 8)!;
  const clip = surround.items.find((i) => i.channels === 8)!;
  expect(clip.effects ?? []).toEqual([]);
  await page.evaluate((id) => window.__aafTimeline!.show(id, 300), clip.object);
  const canvas = (await page.locator(".timeline canvas").boundingBox())!;
  const box = await clipBox(editor, clip.object);
  expect(box.y + box.height).toBeLessThanOrEqual(canvas.y + canvas.height);
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await expect(page.locator(".panel .subtitle")).toContainText(`OperationGroup · object ${clip.object}`);
  await expect(page.locator(".timeline .chain")).toContainText("7.1_01-01.L");
});

test("lifting a rendered crossfade replaces it with a cut and says so", async ({ editor }) => {
  const { page } = editor;
  await editor.open(editor.copy("protools/multichannel_frame_aligned.aaf"));
  await expect.poll(async () => (await tracks(editor)).some((t) => t.items.some((i) => i.rendered === "crossfade"))).toBe(true);
  const track = (await tracks(editor)).find((t) => t.kind === "sound" && !t.channels && t.items.some((i) => i.rendered === "crossfade"))!;
  const fade = track.items.find((i) => i.rendered === "crossfade")!;
  await page.evaluate(() => window.__aafTimeline!.zoom(8));
  await page.evaluate((id) => window.__aafTimeline!.show(id, 300), fade.object);
  const box = await clipBox(editor, fade.object);
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await page.getByRole("button", { name: "Lift" }).click();
  await expect(page.locator(".message.info")).toContainText("Replaced a rendered crossfade with a cut.");
  await expect.poll(async () => (await tracks(editor)).find((t) => t.slot === track.slot)!.items.some((i) => i.rendered)).toBe(false);
});
