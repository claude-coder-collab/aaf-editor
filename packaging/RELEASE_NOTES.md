AAF Editor @VERSION@: a desktop editor (`aafedit`) and command-line tool (`aaftool`) for AAF files.

## Downloads

| Platform | File |
|---|---|
| Linux (Debian 13+, Ubuntu 24.04+) | `aaf-editor-@VERSION@-linux-x86_64.deb` (recommended) or `.tar.gz` |
| macOS 13.3+ (Apple silicon and Intel) | `aaf-editor-@VERSION@-macos-universal.dmg`, plus `aaftool-@VERSION@-macos-universal.tar.gz` for the CLI |
| Windows 10/11 (x64) | `aaf-editor-@VERSION@-windows-x86_64.zip` (portable) |

Verify downloads with `SHA256SUMS.txt`.

## Installing

These builds are **not code-signed or notarized**.

- **Linux:** `sudo apt install ./aaf-editor-@VERSION@-linux-x86_64.deb` installs `aafedit`, `aaftool`, WebKitGTK 4.1 and a desktop entry. The `.tar.gz` holds the same files under `usr/`; it needs `libwebkit2gtk-4.1-0` installed.
- **macOS:** open the `.dmg` and drag `aafedit.app` to Applications. Because the app is unsigned, the first time you open it, right-click it and choose **Open**, or run `xattr -dr com.apple.quarantine /Applications/aafedit.app`. For the CLI, extract `aaftool` from the tarball and run `xattr -d com.apple.quarantine aaftool` if macOS blocks it.
- **Windows:** extract the `.zip` anywhere and run `bin\aafedit.exe`. If SmartScreen warns about an unrecognized app, choose **More info → Run anyway**. The editor uses the Microsoft Edge WebView2 runtime that ships with Windows 10 and 11.

See the README for features and usage.
