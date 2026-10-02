# Changelog

## Unreleased

### New

- **Quick Look previews on macOS.** Select an `.aaf` file in Finder and press Space to see who created and last modified it, the main composition's timeline (zoomable from 1× to 16×) and clip list with record timecodes, and the file's other compositions, master clips and sources. Light and dark mode are supported, and large files stay quick: a 60,000-clip, 30-track sequence previews in under half a second. The extension is included in `aafedit.app`; open the app once to enable it.
- **Application icon** on all platforms: an edit timeline with picture and sound tracks and a playhead.
- `aaftool preview <file> [<out.html>]` writes the same preview page on any platform.
- **Multichannel audio.** Stereo, 5.1 and 7.1 tracks exported from Pro Tools as multichannel tracks are shown as such: each clip appears once, with its name and a format badge, rather than as an "Audio Channel Combiner" effect, and track headers show the format. Splitting and trimming a multichannel clip edit every channel together. The Quick Look preview and its clip list show multichannel clips and track formats too.

### Fixes

- Trimming a clip made of several channels changed only the outer clip, leaving its channels at the old length. Every channel is now trimmed, and validation reports (and the editor refuses) a channel whose length differs from its clip.

## 0.3.0

### Inspector

- **Identifiers are shown as what they point to.** A source clip's SourceID now shows the mob it refers to as a link, and its SourceMobSlotID the slot ("A1 (Slot 1)"). The same applies to EssenceData and rendering MobIDs, linked slot IDs, effect parameter definitions, property types and the Generation of every object (the application that last changed it). The raw identifier is still shown and editable. Targets that are not in the file are labelled "not in this file" rather than flagged as errors, since they may be in another file.
- These identifiers have a **Change…** list, like weak references, so a clip can be pointed at another mob without pasting a MobID.
- A **Referenced by** list shows every object that refers to the selected one, by weak reference or by identifier.
- Changing a MobID, SlotID or definition identifier that other objects use asks whether to update them as well.
- Deleting an object that clips or parameters still refer to by identifier now warns and offers "delete anyway", as it already did for weak references.
- Mob slots are labelled "Name (Slot n)" and Identification objects by product name and version, instead of by class name.
- **SMPTE labels are named.** AUIDs that are SMPTE Universal Labels (compression schemes, essence containers, operational patterns, channel assignments, data definitions and the rest of the SMPTE Labels register) show their registered name, such as "MXF OP1a SingleItem SinglePackage UniTrack Stream Internal" or "Picture Essence Track". A **Change…** button offers the related labels in a list you can filter by typing. Search also matches label names.

## 0.2.1

### Timeline

- **Clips inside effects are shown as clips.** A clip wrapped in effects such as Audio Gain, Pan or colour correction is drawn as the clip, with an "fx" badge naming the effects, instead of as a block labelled with the effect. Clicking the clip selects the clip; clicking the badge selects the effect so its parameters can be edited. The tooltip lists the effects.
- Splitting and trimming such clips keep the effects: both halves of a split keep them, and lengths and source offsets are updated through the effects to the clip. Clips with speed changes or keyframed effect parameters can still be moved, lifted and deleted, but splitting or trimming them is refused with an explanation, because it would change their timing.

### Fixes

- Effect and transition names were blank ("Effect") in Avid files, which call the property that links an effect to its definition "OperationDefinition" rather than "Operation". Property and class lookups now accept the standard name when a file renames it.
- Splitting failed for Avid effects, whose parameters are stored as a keyed set rather than a list.
- Effects are labelled with their effect name in the tree and property panel instead of "OperationGroup".

## 0.2.0

### Faster on large files

On a 60,000-clip, 30-track composition:

- **Opening and saving.** Opening takes 0.3 s instead of 4.3 s, and saving 0.4 s instead of 4.2 s. Name lookups in the compound-file layer were quadratic in the number of clips on a track.
- **Timeline after an edit.** Only the tracks an edit changed are re-projected and sent, so the timeline updates in under 0.1 s instead of 1.5–2 s. The mob list is fetched again only when an edit can change it.
- **Opening a timeline.** The timeline is sent in a compact form, 7 MB instead of 20 MB, and appears about 0.9 s after opening the file instead of about 5.7 s.
- **Drawing.** The timeline redraws in a few milliseconds at any zoom, instead of 50–375 ms. Only visible clips are drawn, clips narrower than a pixel are merged, and hit testing uses the same index.

### Fixes

- Moving or placing a clip onto a track added with **+V** or **+A** was refused in files that use the legacy (Avid) picture and sound data definitions. Legacy and SMPTE definitions of the same kind are now interchangeable, and new tracks use the data definition the mob's other tracks already use.

### Changes for scripts

- `timeline.get` (through `aaftool rpc` or the editor's RPC) returns a compact form: referenced sources are listed once in `sources` and items refer to them by index, and default fields are omitted. It also accepts `changed: [object ids]` to return only the affected tracks (SPEC §8.3). `aaftool timeline --json` is unchanged.
- New `aaftool serve`: JSON-RPC requests on stdin, one response and its events per line on stdout.

### Development

- Playwright end-to-end tests drive the real editor page against `aaftool serve`: Chromium and WebKit on Linux, WebKit on macOS, and Edge on Windows.
- Performance tools: `tools/gen_stress_aaf.py`, `tools/perf_baseline.py`, and limits in `tools/perf_limits.json`, checked on every push to main. Results are in `docs/performance.md`.
- CI: no nightly schedule. Fuzzing has moved to `fuzz.yml` and runs when parser code changes, or manually.

## 0.1.0

First release: the `aaf` library (compound files, the stored format, metamodel, editing with undo, and the timeline projection and operations), `aaftool`, and the `aafedit` desktop editor with an object inspector and a timeline.
