# Automatic publishing

Every change made in the ESP32-206 development session is versioned and pushed
without being asked. This page describes the machine that does it, what it will
and will not publish, and how to read it when it stops.

## Why not a plugin

DSH plugins are packages installed into a *profile* — they extend the interface
and the toolset. There is no hook that says "a file changed, go publish it", and
a plugin would only run while the harness is open.

What runs instead is a **Windows scheduled task**, `watch-esp32-publisher`, that
calls `tools/publish/publish-session.ps1` every 5 minutes. It is independent of
any session, of the interface, and of which agent is driving.

```powershell
# install / remove
.\tools\publish\install-publisher.ps1
.\tools\publish\install-publisher.ps1 -Remove

# one cycle by hand
.\tools\publish\publish-session.ps1
.\tools\publish\publish-session.ps1 -DryRun      # show the plan, change nothing
.\tools\publish\publish-session.ps1 -Force       # ignore the settle delay
```

Configuration lives in `tools/publish/publish.config.json`, **untracked** — the
repository never carries a home directory. Copy
`publish.config.example.json` and point it at your machine.

## What gets published

| From the session | To the repository |
|---|---|
| `CMakeLists.txt`, `partitions.csv`, `sdkconfig*`, `dependencies.lock` | same paths |
| `main/**` | `main/**` (deletions propagated) |
| `components/esp_brookesia/src/**` + the component's top-level files | same paths |
| `tools/{make_icons,png2lvgl,snap,capture,mock_xiaozhi}.py` | `tools/` |
| `backup/vX.Y/*0x200000.bin` | `firmware/vX.Y/{montre,bootloader,partition-table}.bin` + `SHA256SUMS` |
| `backup/VERSIONS.md` | `CHANGELOG.md`, `VERSION`, tag, release |

Deliberately **not** published: `build/`, `managed_components/` (250 MB), serial
logs, screen dumps, the temporary `main/xz_test.h`, and the local skill bundle —
its content is already in `docs/`, minus the personal paths it contains.

## Two modes, and the rule that governs them

The version number is **never invented**. It comes from the session's own
`backup/VERSIONS.md`, which is where the human-readable decision lives.

**Version mode.** If `VERSIONS.md` declares a version whose `MAJOR.MINOR` is
greater than the repository's `VERSION`, the cycle publishes a release:

1. mirrors the sources;
2. copies the image to `firmware/vX.Y/`, with bootloader and partition table
   inherited from the previous release when no local `build/` is available;
3. inserts `## [X.Y.0]` in `CHANGELOG.md`, with the session's own notes;
4. writes `VERSION`, commits, tags `vX.Y.0`, pushes both;
5. creates the GitHub release with the image attached.

**Snapshot mode.** Any other change becomes a commit whose `CHANGELOG` entry
lives under `## [Unreleased]` — a rolling summary, not a history: the history is
git. No version bump, no tag, no release. It records that the session moved.

That division is the point: **the session decides what a version is**, the
publisher only makes sure nothing is lost and everything that was decided is
public.

## Guards

A publisher that pushes to a public repository unattended needs to fail closed.

| Guard | Behaviour |
|---|---|
| Working tree must be clean | Any tracked or untracked change is treated as "someone is working", and the cycle does nothing |
| Settle delay | No publish while a watched file changed less than 3 minutes ago (half-saved file, build in progress) |
| Secret scan | Staged **text** files are scanned; a match aborts and restores the tree. Binaries are excluded on purpose: a firmware legitimately embeds the mbedTLS PEM type strings |
| Atomic rollback | A cycle that aborts after touching files runs `git reset` + `checkout` + `clean` on the paths it owns, so the publisher cannot wedge itself |
| One cycle at a time | Global mutex |
| Push failure | The commit stays local, `pendingPush` is set, the next cycle retries the push before doing anything else |
| Release safety | A release is only created when `origin` is the canonical `withinaz/watch-esp32`; a clone can never publish onto it |

## Reading it

```powershell
Get-Content "$env:LOCALAPPDATA\watch-esp32-publisher\publish.log" -Tail 30
Get-Content "$env:LOCALAPPDATA\watch-esp32-publisher\state.json"
```

The log is append-only, rotated at 1 MB. Every line is one decision:

```
cycle: mode=version repo=3.5.0 session=v3.6 fingerprint=f005d75fcde7
inherited bootloader.bin from firmware/v3.5
published v3.6.0 (0 file(s), 3486160 bytes)
```

`state.json` holds the last published fingerprint, the version, and
`pendingPush`. Deleting it forces a re-evaluation on the next cycle.

## What it does not do

- It does not touch `docs/versions.md` or the README: those stay curated by hand.
- It does not decide that a change deserves a version.
- It does not run when nobody is logged on — the task uses the interactive
  token, because it needs the git and GitHub credentials of the user.
- It does not build. If the session did not compile, the published image is
  whatever was archived, and that is the session's responsibility.
