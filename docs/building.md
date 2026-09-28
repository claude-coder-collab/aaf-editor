# Building and testing

## Requirements

- CMake 3.28 or later, and Ninja.
- A C++26 compiler. CI uses GCC 14, Clang 22, MSVC 19.51 and the current Xcode's AppleClang.
- Node.js 20 or later (CI uses 24) to build the editor UI.
- Python 3.11 or later for the tools in `tools/`. Install their dependencies with `pip install -r tools/requirements.txt`.
- Linux only: WebKitGTK 4.1 and GTK 3 development files (`libwebkit2gtk-4.1-dev` on Debian and Ubuntu).

## Presets

The configure presets `gcc`, `clang` and `msvc` use the Ninja Multi-Config generator, with the build presets
`<compiler>-debug` and `<compiler>-release`:

```
cmake --preset clang
cmake --build --preset clang-debug
ctest --preset clang-debug
```

The sanitizer presets are `clang-asan` (AddressSanitizer and UndefinedBehaviorSanitizer) and `clang-tsan`
(ThreadSanitizer). `clang-fuzz` builds the libFuzzer targets `fuzz_cfb` and `fuzz_document`.

## Options

| Option | Default | Meaning |
|---|---|---|
| `AAF_BUILD_TESTS` | ON | Catch2 unit tests. |
| `AAF_BUILD_EDITOR` | ON | The `aafedit` desktop editor. It is skipped with a warning when npm, or WebKitGTK on Linux, is missing. |
| `AAF_UI_DIST` | — | A directory with a prebuilt `index.html`, used instead of building the UI with npm. |
| `AAF_WARNINGS_AS_ERRORS` | OFF | `-Werror` or `/WX`; CI turns it on. |
| `AAF_VERSION` | the project version | The version embedded in binaries and packages. |

## UI development

The UI in `ui/` is Svelte 5 and TypeScript. `npm ci` installs the pinned dependencies, and
`npm run check && npm test` type-checks and runs the Vitest tests. CMake runs `npm run build` and embeds the resulting
single `index.html` in `aafedit`.

## Reference fixtures

The six AAF SDK samples in `tests/fixtures/aafsdk` are committed. The pyaaf2 and OpenTimelineIO samples are
downloaded and checked against pinned SHA-256 hashes:

```
python3 tools/fetch_fixtures.py
```

Tests that need them are tagged `[external]` and are skipped when the files are absent.

## Cross-checks

These scripts compare our results with pyaaf2 and OpenTimelineIO. They run nightly in CI.

```
python3 tools/crosscheck_cfb.py <aaftool>          # container rewrite
python3 tools/crosscheck.py <aaftool>              # object model
python3 tools/crosscheck.py --roundtrip <aaftool>  # saved files
python3 tools/crosscheck.py --essence <aaftool>    # embedded media
python3 tools/crosscheck_edits.py <aaftool>        # timeline edits, including OTIO's reading of them
```

## Packaging

`cpack -C Release` in a build directory creates the platform packages:

- `.deb` and `.tar.gz` on Linux;
- `.dmg` on macOS;
- `.zip` on Windows.

The `Release` workflow builds them for every `v*` tag and publishes a GitHub release with checksums.
