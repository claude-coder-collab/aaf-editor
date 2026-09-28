# Performance baseline

Measured on 2026-09-28 with a Release build on a Linux x86-64 development machine. Use the numbers to compare
stages and changes with each other, not as absolute targets.

## Test file

`tools/gen_stress_aaf.py stress.aaf` with its defaults: one composition with 2 picture and 28 sound tracks, each
holding 2,000 source clips with occasional fillers, referencing 500 master mobs. That is 60,000 clips (62,951
timeline items with the fillers), 66,408 objects and 26 MB. This is the size of a long-form cut with a full
audio layout.

## Core (`tools/perf_baseline.py`, through `aaftool serve`)

| Stage | Baseline | CFB fixes | Incremental | Response now |
|---|---:|---:|---:|---:|
| doc.open | 4,330 ms | 332 ms | 339 ms | — |
| timeline.mobs | 152 ms | 152 ms | 67 ms | 87 KB |
| timeline.get (whole composition) | 1,367 ms | 1,382 ms | 900 ms | 20.2 MB |
| timeline.op split | 95 ms | 90 ms | 65 ms | 1 KB |
| timeline.get after the edit | 1,348 ms | 1,368 ms | 861 ms | 20.2 MB |
| timeline.get of the changed track only | — | — | 84 ms | 675 KB |
| edit.undo | 37 ms | 31 ms | 14 ms | 1 KB |
| doc.validate | 232 ms | 225 ms | 213 ms | — |
| doc.saveAs | 4,178 ms | 412 ms | 415 ms | — |

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

## Native editor (WebKitGTK, timed inside the page through the real bridge)

| Call | Baseline | Now |
|---|---:|---:|
| timeline.get (62,951 items) | 1,277 ms | 1,145 ms |
| timeline.op addTrack | 111 ms | 39 ms |
| refresh after the edit | 1,355 ms (whole timeline) | 68 ms (changed track only) |

The native bridge adds little to the core's time, even for the 20 MB response.

## Timeline canvas (Playwright, in-page)

| | Chromium | WebKit |
|---|---:|---:|
| draw at Fit (all 62,951 items in view) | 200–228 ms | 366 ms |
| draw zoomed in | 50–56 ms | 144 ms |
| hit test (one mouse move) | 1.3–1.4 ms | 2.4–2.7 ms |
| JS heap after loading | 497 MB | — |

The Playwright end-to-end times are dominated by Playwright copying responses between Node and the browser, and do
not reflect the native editor. Opening to the timeline takes 12–15 s there. Edit to redraw fell from 14–21 s at the
baseline to 0.9–1.4 s with incremental refresh.

## What this means

After an edit on this file, the native editor now spends about 0.1–0.2 s on the edit, the changed-track refresh and the mob list together. Drawing
the updated timeline dominates, at 0.2–0.4 s per frame at Fit zoom. Scrolling, hovering and dragging at Fit still
stutter for the same reason.

## Recommended next steps, by expected gain

1. ~~**Send only what changed.**~~ Done (PR #14).
2. **Leaner projection payload.** The payload is 320 bytes per item; most of it is repeated strings (class names,
   MobIDs, kinds). Interning them per response, or a compact array form, should cut it several-fold and speed up both
   serialisation and parsing.
3. **Level of detail when drawing.** At Fit, most clips are narrower than a pixel. Merging adjacent sub-pixel items
   into one rectangle per pixel column, and caching `getComputedStyle` colours once per frame instead of once per
   item, should bring Fit drawing well under 16 ms. Drawing while zoomed in also loops over every item; a binary
   search per row for the visible range fixes that.
4. **Hit testing** is already fast enough (about 1–3 ms), but the same per-row binary search would make it
   constant-time.
5. **Mob list after edits.** `timeline.mobs` (67 ms now) is refetched whenever an edit touches a mob or creates an
   object, which covers most timeline edits. Refetching only when a mob's name, kind or usage changes avoids it.
6. **Regression guard.** Once the above lands, add a CI job that generates a smaller stress file (for example 10
   tracks × 1,000 clips) and fails if open, timeline.get or draw exceed a generous limit.
