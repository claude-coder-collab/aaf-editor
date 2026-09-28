# Performance baseline

Measured on 2026-09-28 with a Release build on a Linux x86-64 development machine. Use the numbers to compare
stages and changes with each other, not as absolute targets.

## Test file

`tools/gen_stress_aaf.py stress.aaf` with its defaults: one composition with 2 picture and 28 sound tracks, each
holding 2,000 source clips with occasional fillers, referencing 500 master mobs. That is 60,000 clips (62,951
timeline items with the fillers), 66,408 objects and 26 MB. This is the size of a long-form cut with a full
audio layout.

## Core (`tools/perf_baseline.py`, through `aaftool serve`)

| Stage | Baseline | CFB fixes | Incremental | Compact payload | Response now |
|---|---:|---:|---:|---:|---:|
| doc.open | 4,330 ms | 332 ms | 339 ms | 336 ms | — |
| timeline.mobs | 152 ms | 152 ms | 67 ms | 73 ms | 87 KB |
| timeline.get (whole composition) | 1,367 ms | 1,382 ms | 900 ms | 271 ms | 7.1 MB |
| timeline.op split | 95 ms | 90 ms | 65 ms | 31 ms | 1 KB |
| timeline.get after the edit | 1,348 ms | 1,368 ms | 861 ms | 279 ms | 7.1 MB |
| timeline.get of the changed track only | — | — | 84 ms | 18 ms | 317 KB |
| edit.undo | 37 ms | 31 ms | 14 ms | 10 ms | 1 KB |
| doc.validate | 232 ms | 225 ms | 213 ms | 205 ms | — |
| doc.saveAs | 4,178 ms | 412 ms | 415 ms | 451 ms | — |

**CFB fixes (PR #13):** both slow stages were quadratic name handling in the CFB layer. An AAF vector of n objects
is stored as n sibling storages, so a 2,000-clip sequence has 2,000 siblings.

- `Container::find` scanned all siblings on every lookup; it now binary-searches (85% of open time).
- `Builder::addNode` scanned all siblings to reject duplicate names; it now uses a hash set (84% of save time).

**Incremental timeline (PR #14):**

- `timeline.get` takes the edit's changed objects and re-projects only the affected tracks.
- The fixed cost of every projection fell because of three changes:
  - `Projector` decides class membership once per class instead of once per object;
  - `MetaModel::findClassByName` uses an index instead of a scan;
  - `MetaModel::isA` no longer allocates a set on every call.

**Compact payload (PR #16):**

- Each referenced source mob is listed once per response and referenced by index.
- Default classes, labels and `hasLength` are omitted.
- The result is written directly as text instead of building a JSON tree and then serialising it.
- The response is 2.8× smaller and 3.3× faster to produce.

## Native editor (WebKitGTK, timed inside the page through the real bridge)

| Call | Baseline | Incremental | Compact payload |
|---|---:|---:|---:|
| doc.open | — | — | 433–448 ms |
| timeline.get (62,951 items), including expansion in the page | 1,277 ms | 1,145 ms | 353–365 ms |
| layout of the expanded timeline | — | — | 6–7 ms |
| timeline.op addTrack | 111 ms | 39 ms | — |
| refresh after the edit | 1,355 ms (whole timeline) | 68 ms (changed track only) | — |

The native bridge adds little to the core's time, even for the 20 MB response.

## Timeline canvas (Playwright, in-page)

| | Chromium before | Chromium now | WebKit before | WebKit now |
|---|---:|---:|---:|---:|
| draw at Fit (all 62,951 items in view) | 200–228 ms | 2.4 ms | 366 ms | 3.9 ms |
| draw zoomed in | 50–56 ms | 0.9 ms | 144 ms | 1.0 ms |
| hit test (one mouse move) | 1.3–1.4 ms | 0.05 ms | 2.4–2.7 ms | 0.04 ms |
| JS heap after loading | 497 MB | — | — | — |

**Drawing (PR #15):**

- CSS colours are read once per frame.
- A per-row index lets drawing and hit-testing visit only the items in view.
- Runs of clips narrower than 2 px are merged into one rectangle.
- Rows out of view are skipped.
- The largest single factor was the timeline being deep reactive Svelte state. Every property read in the draw loop went through a proxy, and making it `$state.raw` cut the Fit draw from about 35 ms to about 2 ms on its own.

The Playwright end-to-end times are dominated by Playwright copying responses between Node and the browser, and do
not reflect the native editor. Opening to the timeline takes 12–15 s there. Edit to redraw fell from 14–21 s at the
baseline to 0.9–1.4 s with incremental refresh, and to 0.25–0.4 s with the drawing changes.

## What this means

After an edit on this file, the native editor spends about 0.1–0.2 s on the edit, the changed-track refresh and the
mob list, and a few milliseconds drawing. Scrolling, zooming, hovering and dragging redraw in under 5 ms at any
zoom, well inside a 60 Hz frame. Opening the file shows the timeline in about 0.9 s: 0.45 s to load, 0.07 s for the mob
list and 0.36 s for the timeline.

## Recommended next steps, by expected gain

1. ~~**Send only what changed.**~~ Done (PR #14).
2. ~~**Leaner projection payload.**~~ Done (PR #16).
3. ~~**Level of detail when drawing.**~~ Done (PR #15).
4. ~~**Hit testing.**~~ Done (PR #15), with the same per-row search.
5. ~~**Mob list after edits.**~~ Done (PR #17). The server marks `doc.changed` with `mobsChanged`, and the UI
   refetches the mob list only then. Splits, trims, lifts and moves no longer cost a `timeline.mobs` call (about
   70 ms on this file).
6. ~~**Regression guard.**~~ Done (PR #17). The nightly `performance` job in `full.yml` generates a 4 × 3,000-clip
   file and checks the limits in `tools/perf_limits.json` for the core stages (time and response size) and for
   drawing and hit testing. With either old quadratic CFB bug put back, open or save takes about 1.3 s against a
   400 ms limit and the job fails. The runner measured about twice as fast as the development machine.
