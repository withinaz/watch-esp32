#!/usr/bin/env python3
"""Repository checks, run by CI and usable locally.

    python tools/verify_repo.py

Verifies, without a toolchain and without touching a board:

  1. VERSION and the top CHANGELOG entry agree;
  2. the firmware directory for that version exists, with every image listed in
     SHA256SUMS matching its hash;
  3. no build output is tracked by git;
  4. the icon arrays are 150 x 150 (22500 words);
  5. every relative link in the Markdown files resolves.

Exit code 0 when everything passes, 1 otherwise.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

REQUIRED_FILES = [
    "README.md",
    "README.fr.md",
    "CHANGELOG.md",
    "ROADMAP.md",
    "VERSION",
    "LICENSE",
    "NOTICE",
    "CMakeLists.txt",
    "partitions.csv",
    "sdkconfig.defaults",
    "main/CMakeLists.txt",
    "main/main.cpp",
    "main/app_settings.cpp",
    "main/app_assistant.cpp",
    "main/app_music.cpp",
    "main/xiaozhi_client.cpp",
    "main/icons/app_icons.h",
    "tools/make_icons.py",
    "docs/architecture.md",
    "docs/pitfalls.md",
]

ICON_TARGET = 150 * 150
errors: list[str] = []
notes: list[str] = []


def fail(message: str) -> None:
    errors.append(message)


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def check_required() -> None:
    for relative in REQUIRED_FILES:
        if not (ROOT / relative).is_file():
            fail(f"missing required file: {relative}")


def current_version() -> str:
    version = read("VERSION").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        fail(f"VERSION is not MAJOR.MINOR.PATCH: {version!r}")
    return version


def check_changelog(version: str) -> None:
    changelog = read("CHANGELOG.md")
    match = re.search(r"^## \[(\d+\.\d+\.\d+)\]", changelog, re.MULTILINE)
    if not match:
        fail("CHANGELOG.md has no '## [x.y.z]' section")
        return
    if match.group(1) != version:
        fail(f"CHANGELOG top entry is {match.group(1)}, VERSION says {version}")
    if f"[{version}]:" not in changelog:
        fail(f"CHANGELOG.md has no link reference for [{version}]")
    notes.append(f"version {version} matches CHANGELOG")


def check_firmware(version: str) -> None:
    major, minor, _patch = version.split(".")
    directory = ROOT / "firmware" / f"v{major}.{minor}"
    if not directory.is_dir():
        fail(f"firmware directory missing for {version}: {directory.relative_to(ROOT)}")
        return

    manifest = directory / "SHA256SUMS"
    if not manifest.is_file():
        fail(f"missing {manifest.relative_to(ROOT)}")
        return

    listed = 0
    for line in manifest.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        match = re.fullmatch(r"([0-9a-f]{64})\s+\*?(\S+)", line)
        if not match:
            fail(f"malformed SHA256SUMS line: {line!r}")
            continue
        expected, name = match.groups()
        image = directory / name
        if not image.is_file():
            fail(f"image listed but absent: firmware/{directory.name}/{name}")
            continue
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        if digest != expected:
            fail(f"sha256 mismatch for {name}: {digest} != {expected}")
        listed += 1

    if listed == 0:
        fail("SHA256SUMS lists no image")
    else:
        notes.append(f"firmware {directory.name}: {listed} images verified")


def check_tracked_files() -> None:
    try:
        output = subprocess.run(
            ["git", "ls-files"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        notes.append(f"git not available, skipping tracked-file check ({exc.__class__.__name__})")
        return

    tracked = output.splitlines()
    for path in tracked:
        if path.startswith(("build/", "managed_components/")):
            fail(f"build output is tracked: {path}")
    for path in tracked:
        if Path(ROOT / path).suffix == ".log":
            fail(f"log file is tracked: {path}")
    notes.append(f"{len(tracked)} tracked files, no build output")


def check_icons() -> None:
    icons = sorted((ROOT / "main" / "icons").glob("app_icon_*.c"))
    if not icons:
        fail("no main/icons/app_icon_*.c found")
        return
    for icon in icons:
        text = icon.read_text(encoding="utf-8")
        match = re.search(r"_map\[(\d+)\]", text)
        if not match:
            fail(f"{icon.name}: no _map[] array")
            continue
        if int(match.group(1)) != ICON_TARGET:
            fail(f"{icon.name}: {match.group(1)} words, expected {ICON_TARGET} (150x150)")
        if "150x150" not in text:
            notes.append(f"{icon.name}: header does not say 150x150")
    notes.append(f"{len(icons)} icons, {ICON_TARGET} words each")


LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def check_links() -> None:
    broken = 0
    for page in sorted(ROOT.rglob("*.md")):
        if any(part in {".git", "components", "managed_components"} for part in page.parts):
            continue
        text = page.read_text(encoding="utf-8")
        for target in LINK_RE.findall(text):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            path_part = target.split("#", 1)[0]
            if not path_part:
                continue
            resolved = (page.parent / path_part).resolve()
            if not resolved.exists():
                fail(f"broken link in {page.relative_to(ROOT)}: {target}")
                broken += 1
    if broken == 0:
        notes.append("all relative Markdown links resolve")


def main() -> int:
    check_required()
    version = current_version()
    check_changelog(version)
    check_firmware(version)
    check_tracked_files()
    check_icons()
    check_links()

    for note in notes:
        print(f"  ok   {note}")
    for error in errors:
        print(f"  FAIL {error}")

    print()
    if errors:
        print(f"{len(errors)} problem(s)")
        return 1
    print("repository verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
