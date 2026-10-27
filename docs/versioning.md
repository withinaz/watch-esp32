# Versioning and releases

## The scheme

`MAJOR.MINOR.PATCH`, with a rule that is specific to this project:

| Bump | When |
|---|---|
| **MAJOR** | an **application** is added to the launcher (v2 → v3 = Music) |
| **MINOR** | a revision of the same application set: display fix, UI change, new behaviour inside an existing app |
| **PATCH** | reserved for a fix that ships without any visible change |

Consequence: v3.5 is *the third generation of applications* with five revisions
of polish. Adding a fourth app is v4.0, not v3.6.

`VERSION` at the repository root holds the current version, and `CHANGELOG.md`
has an entry for every published increment. Both are checked by CI against the
latest git tag — they cannot drift.

## What is published

Each published increment is:

1. a **tag** `vX.Y.Z` on a commit that builds;
2. a **CHANGELOG entry** describing what changed and, when relevant, what was
   measured;
3. a **verified binary** in `firmware/vX.Y/` with `SHA256SUMS`, unless the
   increment does not change the application image;
4. a **release note** on GitHub, written for someone who owns the watch: what
   they will see, how to flash, how to roll back.

A fix that does not survive a real flash on the board is not a release. A
release note never claims something that was not verified on the watch, and when
something remains broken it says so in the note.

## Release checklist

```powershell
# 1. build and check
idf.py build
idf.py size

# 2. flash the exact image you are about to publish
python -m esptool --chip esp32s3 -p COM4 -b 921600 write_flash 0x200000 build\montre.bin

# 3. read the boot log and exercise the change by hand
python tools\capture.py COM4 serial.log 20

# 4. publish
#    - bump VERSION and CHANGELOG
#    - copy build\montre.bin to firmware\vX.Y\, regenerate SHA256SUMS
#    - .\firmware\flash.ps1 -DryRun        (verifies the manifest)
#    - commit, tag, push
git tag -a v3.5.0 -m "v3.5.0 - top band repaint"
git push origin main --tags

# 5. release note
gh release create v3.5.0 firmware\v3.5\montre.bin --title "v3.5.0" --notes-file notes.md
```

## Tag format

Annotated tags, `vMAJOR.MINOR.PATCH`:

```
v1.0.0  v2.0.0  v2.1.0  v2.2.0  v2.3.0  v3.0.0
v3.1.0  v3.2.0  v3.3.0  v3.4.0  v3.5.0
```

The firmware inside a tag must be reproducible from the sources of that tag —
that is the point of the tag. A binary that cannot be rebuilt is not a release
artefact, it is a file.

## On-demand increments

New applications, new tools and new skills are published **when they are
asked for**, one increment at a time, each with its own tag and changelog entry.
No version is published from a work-in-progress branch, and no tag is moved once
pushed: a mistake is fixed by the next patch version, never by rewriting a tag.

## Branches

`main` is the only long-lived branch and is always flashable. Work in progress
lives on short branches named after the increment (`v4-clock-app`) and is merged
only after being tested on the watch.

## Deprecations

A version is never deleted from the history, even when it was a bad idea — see
v3.6 in [versions.md](versions.md). What can be removed is the **binary**, when
it was never a keeper, and the entry then says so explicitly.
