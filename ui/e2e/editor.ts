import { spawn, spawnSync, type ChildProcessWithoutNullStreams } from "node:child_process";
import { copyFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath, pathToFileURL } from "node:url";

import { test as base, expect, type Page } from "@playwright/test";

import type {} from "../src/lib/rpc";

const here = dirname(fileURLToPath(import.meta.url));
export const repoRoot = resolve(here, "..", "..");
export const fixture = (relative: string) => resolve(repoRoot, "tests", "fixtures", relative);

export function aaftoolPath(): string {
  const path = process.env.AAFTOOL;
  if (!path) throw new Error("Set AAFTOOL to the aaftool executable");
  return resolve(path);
}

export function aaftool(...args: string[]): { status: number | null; stdout: string } {
  const run = spawnSync(aaftoolPath(), args, { encoding: "utf8" });
  return { status: run.status, stdout: run.stdout };
}

interface Event {
  method: string;
  params: unknown;
}

/// Runs `aaftool serve` and answers one JSON-RPC request at a time.
class Core {
  private readonly process: ChildProcessWithoutNullStreams;
  private readonly waiting: ((line: string) => void)[] = [];
  private queue = Promise.resolve();

  constructor() {
    this.process = spawn(aaftoolPath(), ["serve"], { stdio: ["pipe", "pipe", "pipe"] });
    createInterface({ input: this.process.stdout }).on("line", (line) => this.waiting.shift()?.(line));
  }

  request(line: string): Promise<{ response: unknown; events: Event[] }> {
    const next = this.queue.then(
      () =>
        new Promise<{ response: unknown; events: Event[] }>((done) => {
          this.waiting.push((out) => done(JSON.parse(out)));
          this.process.stdin.write(line.replace(/\n/g, " ") + "\n");
        }),
    );
    this.queue = next.then(() => undefined);
    return next;
  }

  close(): void {
    this.process.stdin.end();
    this.process.kill();
  }
}

/// What the native host's dialogs return and what it was asked to do.
export interface Host {
  openPath: string | null;
  savePath: string | null;
  titles: string[];
}

export interface Editor {
  page: Page;
  host: Host;
  /// A private copy of a fixture, so tests can save over it.
  copy: (relative: string) => string;
  /// Opens a file through the toolbar's Open button.
  open: (path: string) => Promise<void>;
}

export const test = base.extend<{ editor: Editor }>({
  editor: async ({ page }, use, testInfo) => {
    const core = new Core();
    const host: Host = { openPath: null, savePath: null, titles: [] };
    await page.addInitScript(() => (window.__aafTest = true));
    await page.exposeFunction("aafRpc", async (request: string) => {
      if (process.env.E2E_LOG) console.log(">>", request.slice(0, 200));
      const { response, events } = await core.request(request);
      if (process.env.E2E_LOG) console.log("<<", JSON.stringify(response).slice(0, 200));
      if (events.length > 0) {
        await page.evaluate((list) => list.forEach((e) => window.__aafEvent?.(e)), events);
      }
      return response;
    });
    await page.exposeFunction("aafHost", (command: string, args: { title?: string }) => {
      if (command === "openDialog") return host.openPath;
      if (command === "saveDialog") return host.savePath;
      if (command === "setTitle") host.titles.push(args.title ?? "");
      return null;
    });
    await page.goto(pathToFileURL(resolve(process.env.AAF_UI_HTML ?? resolve(here, "..", "dist", "index.html"))).href);
    const editor: Editor = {
      page,
      host,
      copy: (relative) => {
        const target = testInfo.outputPath(relative.split("/").at(-1)!);
        copyFileSync(fixture(relative), target);
        return target;
      },
      open: async (path) => {
        host.openPath = path;
        await page.getByRole("button", { name: "Open…" }).first().click();
        await expect(page.locator(".toolbar .file")).toContainText(path.split(/[\\/]/).at(-1)!);
      },
    };
    await use(editor);
    core.close();
  },
});

export { expect };
