#!/usr/bin/env python3
"""Check the macOS Quick Look extension embedded in aafedit.app.

Static checks (always): the app and extension signatures verify (deep, strict), the extension is sandboxed
with read access to user-selected files, and its Info.plist declares a data-based Quick Look preview for the
system AAF type.

With `--smoke FILE...`: copies the app to ~/Applications, registers the extension, previews each file with
`qlmanage -p` and requires the extension to log "AAF preview rendered" once per file without crashing.
The copy is removed afterwards unless `--keep`.
"""

from __future__ import annotations

import argparse
import plistlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

EXTENSION = "Contents/PlugIns/AAFPreview.appex"
BUNDLE_ID = "io.github.claude-coder-collab.aaf-editor.quicklook"
AAF_TYPE = "org.aafassociation.advanced-authoring-format"
LSREGISTER = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"


class CheckError(Exception):
    pass


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise CheckError(f"{' '.join(args)} failed:\n{result.stdout}{result.stderr}")
    return result


def entitlements(bundle: Path) -> dict[str, object]:
    out = subprocess.run(["codesign", "-d", "--entitlements", "-", "--xml", str(bundle)], capture_output=True, check=True)
    return plistlib.loads(out.stdout) if out.stdout.strip() else {}


def check_static(app: Path) -> None:
    appex = app / EXTENSION
    if not appex.is_dir():
        raise CheckError(f"{appex} is missing")
    run("codesign", "--verify", "--deep", "--strict", str(app))
    granted = entitlements(appex)
    for key in ("com.apple.security.app-sandbox", "com.apple.security.files.user-selected.read-only"):
        if granted.get(key) is not True:
            raise CheckError(f"the extension lacks the {key} entitlement")
    info = plistlib.loads((appex / "Contents/Info.plist").read_bytes())
    extension = info.get("NSExtension", {})
    attributes = extension.get("NSExtensionAttributes", {})
    if info.get("CFBundleIdentifier") != BUNDLE_ID:
        raise CheckError(f"unexpected bundle identifier {info.get('CFBundleIdentifier')}")
    if extension.get("NSExtensionPointIdentifier") != "com.apple.quicklook.preview":
        raise CheckError("the extension is not a Quick Look preview extension")
    if attributes.get("QLIsDataBasedPreview") is not True or AAF_TYPE not in attributes.get("QLSupportedContentTypes", []):
        raise CheckError("the extension does not declare a data-based preview for AAF files")
    if not (app / "Contents/Resources/aafedit.icns").is_file():
        raise CheckError("the app icon is missing")
    print(f"{app}: signature, entitlements and Info.plist OK")


def log_lines(since: str) -> list[str]:
    out = run("/usr/bin/log", "show", "--start", since, "--style", "compact", "--predicate", 'process == "AAFPreview" AND eventMessage CONTAINS "AAF preview rendered"', check=False)
    return [line for line in out.stdout.splitlines() if "AAF preview rendered" in line]


def smoke(app: Path, files: list[Path], keep: bool) -> None:
    installed = Path.home() / "Applications" / app.name
    installed.parent.mkdir(exist_ok=True)
    shutil.rmtree(installed, ignore_errors=True)
    run("ditto", str(app), str(installed))
    try:
        run(LSREGISTER, "-f", "-R", str(installed))
        run("pluginkit", "-a", str(installed / EXTENSION))
        for _ in range(10):
            if str(installed) in run("pluginkit", "-m", "-v", "-i", BUNDLE_ID, check=False).stdout:
                break
            time.sleep(1)
        else:
            raise CheckError("pluginkit did not register the extension")
        run("qlmanage", "-r", check=False)
        since = time.strftime("%Y-%m-%d %H:%M:%S")
        for file in files:
            preview = subprocess.Popen(["qlmanage", "-p", str(file)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(8)
            preview.kill()
            preview.wait()
        lines = log_lines(since)
        for line in lines:
            print(line)
        if len(lines) < len(files):
            raise CheckError(f"expected {len(files)} rendered previews, the extension logged {len(lines)}")
        crashes = sorted((Path.home() / "Library/Logs/DiagnosticReports").glob("AAFPreview*"))
        if crashes:
            raise CheckError(f"the extension crashed: {', '.join(str(c) for c in crashes)}")
        print(f"Quick Look rendered {len(files)} file(s)")
    finally:
        if not keep:
            run("pluginkit", "-r", str(installed / EXTENSION), check=False)
            run(LSREGISTER, "-u", str(installed), check=False)
            shutil.rmtree(installed, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("app", type=Path, help="path to aafedit.app")
    parser.add_argument("--smoke", nargs="+", type=Path, metavar="FILE", help="AAF files to preview through Quick Look")
    parser.add_argument("--keep", action="store_true", help="leave the app installed in ~/Applications after --smoke")
    args = parser.parse_args(argv)
    try:
        check_static(args.app)
        if args.smoke:
            smoke(args.app, args.smoke, args.keep)
    except CheckError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
