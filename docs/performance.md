# Performance baseline

Measured on 2026-09-28 with a Release build on a Linux x86-64 development machine. Use the numbers to compare
stages and changes with each other, not as absolute targets.

## Test file

`tools/gen_stress_aaf.py stress.aaf` with its defaults: one composition with 2 picture and 28 sound tracks, each
holding 2,000 source clips with occasional fillers, referencing 500 master mobs. That is 60,000 clips (62,951
timeline items with the fillers), 66,408 objects and 26 MB. This is the size of a long-form cut with a full
audio layout.

## Core (`tools/perf_baseline.py`, through `aaftool serve`)

| Stage | Before | After | Response |
|---|---:|---:|---:|
| doc.open | 4,330 ms | 332 ms | — |
| timeline.mobs | 152 ms | 152 ms | 87 KB |
| timeline.get (whole composition) | 1,367 ms | 1,382 ms | 20.2 MB |
| timeline.op split | 95 ms | 90 ms | 1 KB |
| timeline.get after the edit | 1,348 ms | 1,368 ms | 20.2 MB |
| edit.undo | 37 ms | 31 ms | 1 KB |
| doc.validate | 232 ms | 225 ms | — |
| doc.saveAs | 4,178 ms | 412 ms | — |

**Fixed in this change:** both slow stages were quadratic name handling in the CFB layer. An AAF vector of n objects
is stored as n sibling storages, so a 2,000-clip sequence has 2,000 siblings.

- `Container::find` scanned all siblings on every lookup; it now binary-searches (85% of open time).
- `Builder::addNode` scanned all siblings to reject duplicate names; it now uses a hash set (84% of save time).

## Native editor (WebKitGTK, timed inside the page through the real bridge)

| Call | Time |
|---|---:|
| timeline.mobs | 122 ms |
| timeline.get (62,951 items) | 1,277 ms |
| timeline.op addTrack | 111 ms |
| timeline.get again | 1,355 ms |

The native bridge adds little to the core's time, even for the 20 MB response.

## Timeline canvas (Playwright, in-page)

| | Chromium | WebKit |
|---|---:|---:|
| draw at Fit (all 62,951 items in view) | 200–228 ms | 366 ms |
| draw zoomed in | 50–56 ms | 144 ms |
| hit test (one mouse move) | 1.3–1.4 ms | 2.4–2.7 ms |
| JS heap after loading | 497 MB | — |

The Playwright end-to-end times (open to timeline shown: 12–15 s, edit to redraw: 14–21 s) are dominated by
Playwright copying 20 MB responses between Node and the browser, and do not reflect the native editor.

## What this means

After an edit on this file, the native editor takes about 1.5–2 s to redraw: 0.1 s for the edit, 0.15 s for the mob
list, 1.35 s to re-project and send the whole timeline, and 0.2–0.4 s to draw. Every frame at Fit zoom takes
0.2–0.4 s, so scrolling, hovering and dragging stutter.

## Recommended next steps, by expected gain

1. **Send only what changed.** After an edit, re-project and send only the affected tracks (the edit's ChangeSet
   already names the objects). That turns 1.35 s and 20 MB into a few milliseconds for a one-track edit. This is the
   largest win.
2. **Leaner projection payload.** The payload is 320 bytes per item; most of it is repeated strings (class names,
   MobIDs, kinds). Interning them per response, or a compact array form, should cut it several-fold and speed up both
   serialisation and parsing.
3. **Level of detail when drawing.** At Fit, most clips are narrower than a pixel. Merging adjacent sub-pixel items
   into one rectangle per pixel column, and caching `getComputedStyle` colours once per frame instead of once per
   item, should bring Fit drawing well under 16 ms. Drawing while zoomed in also loops over every item; a binary
   search per row for the visible range fixes that.
4. **Hit testing** is already fast enough (about 1–3 ms), but the same per-row binary search would make it
   constant-time.
5. **Mob list after edits.** `timeline.mobs` (150 ms) is refetched whenever an edit touches a mob or creates an
   object, which covers most timeline edits. Refetching only when a mob's name, kind or usage changes avoids it.
6. **Regression guard.** Once the above lands, add a CI job that generates a smaller stress file (for example 10
   tracks × 1,000 clips) and fails if open, timeline.get or draw exceed a generous limit.
