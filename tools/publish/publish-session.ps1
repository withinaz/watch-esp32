<#
.SYNOPSIS
    Publishes the ESP32-206 development session into the watch-esp32 repository.

.DESCRIPTION
    One cycle, meant to be run by Windows Task Scheduler every few minutes:

      1. takes a fingerprint of the watched sources (content hashes), of
         backup\VERSIONS.md and of the newest firmware image;
      2. does nothing if that fingerprint is the one already published;
      3. if backup\VERSIONS.md declares a version newer than the repository's
         VERSION (major.minor), publishes it as a release: sources, firmware
         image, CHANGELOG entry, VERSION bump, tag, GitHub release;
      4. otherwise publishes a snapshot: sources only, recorded in the
         CHANGELOG under [Unreleased], no version bump, no tag.

    The version number itself is never invented: it comes from the session's
    own VERSIONS.md. Only the session decides what a version is.

.PARAMETER ProjectPath
    The ESP-IDF project directory of the watch.

.PARAMETER BackupPath
    Directory holding VERSIONS.md and the vX.Y\ folder of archived images.

.PARAMETER RepoPath
    The watch-esp32 git working tree (default: two levels above this script).

.PARAMETER StatePath
    JSON state file. Defaults to %LOCALAPPDATA%\watch-esp32-publisher.

.PARAMETER SettleMinutes
    Do not publish while a watched file changed less than this many minutes
    ago. Protects against publishing a half-saved file or a build in progress.

.EXAMPLE
    .\publish-session.ps1 -DryRun
    .\publish-session.ps1 -Force
#>
[CmdletBinding()]
param(
    [string] $ProjectPath = '',
    [string] $BackupPath  = '',
    [string] $RepoPath    = '',
    [string] $StatePath   = '',
    [string] $LogPath     = '',
    [int]    $SettleMinutes = 3,
    [switch] $DryRun,
    [switch] $Force,
    [switch] $Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# $PSScriptRoot is empty in param() defaults under Windows PowerShell 5.1 when
# the script has [CmdletBinding()]; resolve everything here instead.
$here = $PSScriptRoot
if (-not $RepoPath)  { $RepoPath  = (Resolve-Path (Join-Path $here '..\..')).Path }
if (-not $StatePath) { $StatePath = Join-Path $env:LOCALAPPDATA 'watch-esp32-publisher\state.json' }
if (-not $LogPath)   { $LogPath   = Join-Path $env:LOCALAPPDATA 'watch-esp32-publisher\publish.log' }

$RepoUrl = 'https://github.com/withinaz/watch-esp32'

# Machine-specific paths live in an untracked config file: this repository must
# never carry somebody's home directory.
$configPath = Join-Path $here 'publish.config.json'
if ((-not $ProjectPath -or -not $BackupPath) -and (Test-Path $configPath)) {
    $config = Get-Content -Path $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $names = $config.PSObject.Properties.Name
    if (-not $ProjectPath -and ($names -contains 'projectPath')) { $ProjectPath = [string]$config.projectPath }
    if (-not $BackupPath  -and ($names -contains 'backupPath'))  { $BackupPath  = [string]$config.backupPath }
}
if (-not $ProjectPath) {
    throw "no project path: pass -ProjectPath or create $configPath (see publish.config.example.json)"
}
if (-not $BackupPath) {
    throw "no backup path: pass -BackupPath or create $configPath (see publish.config.example.json)"
}

# --------------------------------------------------------------------------- log

function Write-Log {
    param([string] $Message, [string] $Level = 'info')

    $line = '{0} {1,-5} {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level.ToUpperInvariant(), $Message
    $dir = Split-Path -Parent $LogPath
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

    if ((Test-Path $LogPath) -and ((Get-Item $LogPath).Length -gt 1MB)) {
        Move-Item $LogPath "$LogPath.1" -Force
    }
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
    if (-not $Quiet) { Write-Host $line -ForegroundColor $(if ($Level -eq 'error') { 'Red' } elseif ($Level -eq 'warn') { 'Yellow' } else { 'Gray' }) }
}

# --------------------------------------------------------------------------- git

$script:GitExe = 'git'

function Initialize-Git {
    $cmd = Get-Command git -ErrorAction SilentlyContinue
    if ($cmd) { $script:GitExe = $cmd.Source; return }

    $fallback = 'C:\Program Files\Git\cmd\git.exe'
    if (Test-Path $fallback) { $script:GitExe = $fallback; return }

    throw 'git not found: neither on PATH nor at C:\Program Files\Git\cmd\git.exe'
}

function Invoke-Git {
    param([string[]] $Arguments, [switch] $AllowFailure)

    $out  = & $script:GitExe -C $RepoPath @Arguments 2>&1
    $code = $LASTEXITCODE
    $text = ($out | ForEach-Object { "$_" }) -join "`n"

    if ($code -ne 0 -and -not $AllowFailure) {
        throw ("git {0} failed ({1}): {2}" -f ($Arguments -join ' '), $code, $text)
    }
    return [pscustomobject]@{ Code = $code; Output = $text }
}

# ----------------------------------------------------------------- what is mirrored

# Everything the public repository owns. Anything else in the project folder
# (build/, managed_components/, logs, snaps, xz_test.h) stays private.
function Get-MirrorEntries {
    $entries = New-Object System.Collections.ArrayList

    foreach ($name in @('CMakeLists.txt', 'partitions.csv', 'sdkconfig', 'sdkconfig.defaults', 'dependencies.lock')) {
        $src = Join-Path $ProjectPath $name
        if (Test-Path $src) {
            [void]$entries.Add([pscustomobject]@{ Source = $src; Relative = $name })
        }
    }

    $mainDir = Join-Path $ProjectPath 'main'
    if (Test-Path $mainDir) {
        Get-ChildItem $mainDir -Recurse -File | Where-Object {
            $_.Name -ne 'xz_test.h' -and $_.Extension -ne '.log'
        } | ForEach-Object {
            $rel = $_.FullName.Substring($ProjectPath.Length + 1) -replace '\\', '/'
            [void]$entries.Add([pscustomobject]@{ Source = $_.FullName; Relative = $rel })
        }
    }

    $srcDir = Join-Path $ProjectPath 'components\esp_brookesia\src'
    if (Test-Path $srcDir) {
        Get-ChildItem $srcDir -Recurse -File | ForEach-Object {
            $rel = $_.FullName.Substring($ProjectPath.Length + 1) -replace '\\', '/'
            [void]$entries.Add([pscustomobject]@{ Source = $_.FullName; Relative = $rel })
        }
    }

    foreach ($name in @('CMakeLists.txt', 'Kconfig', 'idf_component.yml', 'esp_brookesia_conf.h',
                        'license.txt', 'README.md', 'CHANGELOG.md')) {
        $src = Join-Path $ProjectPath "components\esp_brookesia\$name"
        if (Test-Path $src) {
            [void]$entries.Add([pscustomobject]@{ Source = $src; Relative = "components/esp_brookesia/$name" })
        }
    }

    foreach ($name in @('make_icons.py', 'png2lvgl.py', 'snap.py', 'capture.py', 'mock_xiaozhi.py')) {
        $src = Join-Path $ProjectPath "tools\$name"
        if (Test-Path $src) {
            [void]$entries.Add([pscustomobject]@{ Source = $src; Relative = "tools/$name" })
        }
    }

    return $entries
}

# ------------------------------------------------------------------- session state

function Get-SessionVersions {
    $file = Join-Path $BackupPath 'VERSIONS.md'
    if (-not (Test-Path $file)) { return @() }

    $versions = New-Object System.Collections.ArrayList
    $current  = $null
    $body     = New-Object System.Collections.ArrayList

    foreach ($line in (Get-Content -Path $file -Encoding UTF8)) {
        if ($line -match '^##\s+v(\d+)\.(\d+)\s*[-\u2014]+\s*(.+?)\s*$') {
            if ($null -ne $current) {
                $current.Body = ($body -join "`n").Trim()
                [void]$versions.Add($current)
            }
            $current = [pscustomobject]@{
                Major = [int]$Matches[1]
                Minor = [int]$Matches[2]
                Title = $Matches[3]
                Body  = ''
            }
            $body.Clear()
        } elseif ($null -ne $current) {
            [void]$body.Add($line)
        }
    }
    if ($null -ne $current) {
        $current.Body = ($body -join "`n").Trim()
        [void]$versions.Add($current)
    }
    return $versions
}

function Get-LatestBackupImage {
    if (-not (Test-Path $BackupPath)) { return $null }

    $best = $null
    foreach ($dir in (Get-ChildItem $BackupPath -Directory | Where-Object { $_.Name -match '^v(\d+)\.(\d+)$' })) {
        $null = $dir.Name -match '^v(\d+)\.(\d+)$'
        $major = [int]$Matches[1]
        $minor = [int]$Matches[2]

        $image = Get-ChildItem $dir.FullName -File -Filter '*0x200000.bin' -ErrorAction SilentlyContinue |
                 Select-Object -First 1
        if (-not $image) { continue }

        if ($null -eq $best -or $major -gt $best.Major -or ($major -eq $best.Major -and $minor -gt $best.Minor)) {
            $best = [pscustomobject]@{
                Major = $major
                Minor = $minor
                Path  = $image.FullName
                Size  = $image.Length
                Hash  = (Get-FileHash $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
    }
    return $best
}

function Get-Fingerprint {
    param($Entries)

    $builder = New-Object System.Text.StringBuilder
    foreach ($entry in ($Entries | Sort-Object Relative)) {
        $hash = (Get-FileHash $entry.Source -Algorithm SHA256).Hash
        [void]$builder.Append($entry.Relative).Append('=').Append($hash).Append("`n")
    }

    $versionsFile = Join-Path $BackupPath 'VERSIONS.md'
    if (Test-Path $versionsFile) {
        [void]$builder.Append('versions=').Append((Get-FileHash $versionsFile -Algorithm SHA256).Hash).Append("`n")
    }

    $image = Get-LatestBackupImage
    if ($null -ne $image) {
        [void]$builder.Append('image=').Append($image.Major).Append('.').Append($image.Minor).Append('=').Append($image.Hash).Append("`n")
    }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    $digest = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($builder.ToString()))
    return ([System.BitConverter]::ToString($digest) -replace '-', '').ToLowerInvariant()
}

function Get-NewestWriteTime {
    param($Entries)

    $newest = [datetime]::MinValue
    foreach ($entry in $Entries) {
        $stamp = (Get-Item $entry.Source).LastWriteTimeUtc
        if ($stamp -gt $newest) { $newest = $stamp }
    }
    $versionsFile = Join-Path $BackupPath 'VERSIONS.md'
    if (Test-Path $versionsFile) {
        $stamp = (Get-Item $versionsFile).LastWriteTimeUtc
        if ($stamp -gt $newest) { $newest = $stamp }
    }
    return $newest
}

# ------------------------------------------------------------------------- state

function Read-State {
    if (-not (Test-Path $StatePath)) {
        return [pscustomobject]@{ fingerprint = ''; version = ''; pendingPush = $false; lastRun = ''; lastMode = '' }
    }
    $raw = Get-Content -Path $StatePath -Raw -Encoding UTF8
    $obj = $raw | ConvertFrom-Json
    foreach ($field in @('fingerprint', 'version', 'pendingPush', 'lastRun', 'lastMode')) {
        if (-not ($obj.PSObject.Properties.Name -contains $field)) {
            $obj | Add-Member -NotePropertyName $field -NotePropertyValue $(if ($field -eq 'pendingPush') { $false } else { '' })
        }
    }
    return $obj
}

function Save-State {
    param($State)

    $dir = Split-Path -Parent $StatePath
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $State.lastRun = (Get-Date).ToString('o')
    $State | ConvertTo-Json -Depth 4 | Set-Content -Path $StatePath -Encoding UTF8
}

# ----------------------------------------------------------------------- mirroring

function Sync-Mirror {
    param($Entries)

    $changed = New-Object System.Collections.ArrayList

    foreach ($entry in $Entries) {
        $target = Join-Path $RepoPath ($entry.Relative -replace '/', '\')
        $folder = Split-Path -Parent $target
        if (-not (Test-Path $folder)) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }

        $identical = (Test-Path $target) -and
                     ((Get-FileHash $target -Algorithm SHA256).Hash -eq (Get-FileHash $entry.Source -Algorithm SHA256).Hash)

        if (-not $identical) {
            if (-not $DryRun) { Copy-Item -LiteralPath $entry.Source -Destination $target -Force }
            [void]$changed.Add($entry.Relative)
        }
    }

    # delete files that disappeared from a fully mirrored tree
    $known = @{}
    foreach ($entry in $Entries) { $known[$entry.Relative] = $true }

    foreach ($tree in @(Join-Path $RepoPath 'main')) {
        if (-not (Test-Path $tree)) { continue }
        Get-ChildItem $tree -Recurse -File | ForEach-Object {
            $rel = $_.FullName.Substring($RepoPath.Length + 1) -replace '\\', '/'
            if (-not $known.ContainsKey($rel)) {
                if (-not $DryRun) { Remove-Item -LiteralPath $_.FullName -Force }
                [void]$changed.Add("$rel (deleted)")
            }
        }
    }

    return $changed
}

# ----------------------------------------------------------------------- firmware

function Copy-FirmwareImage {
    param($Image)

    if ($null -eq $Image) { return $null }

    $folderName = 'v{0}.{1}' -f $Image.Major, $Image.Minor
    $folder = Join-Path $RepoPath "firmware\$folderName"
    if (-not (Test-Path $folder)) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }

    if (-not $DryRun) { Copy-Item -LiteralPath $Image.Path -Destination (Join-Path $folder 'montre.bin') -Force }

    # bootloader and partition table rarely change: take the fresh build when it
    # exists, otherwise inherit the previous release's copy, so that a version
    # published without a local build/ directory is still flashable with -Full.
    foreach ($pair in @(@('bootloader', 'bootloader.bin'), @('partition-table', 'partition-table.bin'))) {
        $target = Join-Path $folder $pair[1]
        $built  = Join-Path $ProjectPath ("build\{0}\{1}" -f $pair[0], $pair[1])

        if (Test-Path $built) {
            if (-not $DryRun) { Copy-Item -LiteralPath $built -Destination $target -Force }
            continue
        }
        if (Test-Path $target) { continue }

        $previousReleases = Get-ChildItem (Join-Path $RepoPath 'firmware') -Directory |
                            Where-Object { $_.Name -match '^v(\d+)\.(\d+)$' -and $_.Name -ne $folderName } |
                            Sort-Object @{ Expression = { [int]([regex]::Match($_.Name, 'v(\d+)').Groups[1].Value) }; Descending = $true },
                                        @{ Expression = { [int]([regex]::Match($_.Name, '\.(\d+)$').Groups[1].Value) }; Descending = $true }

        foreach ($previous in $previousReleases) {
            $candidate = Join-Path $previous.FullName $pair[1]
            if (-not (Test-Path $candidate)) { continue }
            if (-not $DryRun) { Copy-Item -LiteralPath $candidate -Destination $target -Force }
            Write-Log ("inherited {0} from firmware/{1}" -f $pair[1], $previous.Name)
            break
        }
    }

    if (-not $DryRun) {
        $lines = foreach ($name in @('bootloader.bin', 'partition-table.bin', 'montre.bin')) {
            $file = Join-Path $folder $name
            if (Test-Path $file) {
                '{0}  {1}' -f (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant(), $name
            }
        }
        Set-Content -Path (Join-Path $folder 'SHA256SUMS') -Value (($lines -join "`n") + "`n") -Encoding ascii -NoNewline
    }

    return $folderName
}

# ---------------------------------------------------------------------- changelog

function Update-Changelog {
    param([string] $Mode, $Version, $Image, $Changed)

    $path = Join-Path $RepoPath 'CHANGELOG.md'
    $text = Get-Content -Path $path -Raw -Encoding UTF8
    $stamp = Get-Date -Format 'yyyy-MM-dd'
    $clock = Get-Date -Format 'yyyy-MM-dd HH:mm'

    $files = @($Changed | Where-Object { $_ -notmatch '\(deleted\)$' })
    $listing = ($files | Select-Object -First 12 | ForEach-Object { '- `' + $_ + '`' }) -join "`n"
    if ($files.Count -gt 12) { $listing += ("`n- ... and {0} more" -f ($files.Count - 12)) }

    if ($Mode -eq 'version') {
        $number = '{0}.{1}.0' -f $Version.Major, $Version.Minor
        $body = $Version.Body.Trim()
        $body = [regex]::Replace($body, '(?m)^---\s*$', '').Trim()

        $section = @"
## [$number] - $stamp

Published automatically from the development session as **v$($Version.Major).$($Version.Minor)**.
Notes below are the session's own, in French.

$body

Image: ``firmware/v$($Version.Major).$($Version.Minor)/montre.bin``,
$('{0:N0}' -f $Image.Size) bytes, SHA-256 ``$($Image.Hash)``, write at ``0x200000``.

"@

        $section = $section.TrimEnd() + "`n`n"

        $anchor = [regex]::Match($text, '(?m)^## \[(?!Unreleased\])')
        if (-not $anchor.Success) { throw 'CHANGELOG.md has no version section to insert before' }
        $text = $text.Insert($anchor.Index, $section)

        $linkLine = "[$number]: $RepoUrl/releases/tag/v$number"
        if ($text -notmatch [regex]::Escape("[$number]:")) {
            $text = [regex]::Replace($text, '(?m)^(\[Unreleased\]:.*)$', "`$1`n$linkLine")
        }
    }
    else {
        $block = @"
## [Unreleased]

Work in progress, mirrored from the development session. Last snapshot: $clock
($($files.Count) file(s) changed).

$listing

"@
        $block = $block.TrimEnd() + "`n`n"

        $match = [regex]::Match($text, '(?sm)^## \[Unreleased\].*?(?=^## \[)')
        if (-not $match.Success) {
            $match = [regex]::Match($text, '(?sm)^## \[Unreleased\].*$')
        }
        if ($match.Success) {
            $text = $text.Remove($match.Index, $match.Length).Insert($match.Index, $block)
        } else {
            throw 'CHANGELOG.md has no [Unreleased] section'
        }
    }

    if (-not $DryRun) { Set-Content -Path $path -Value $text -Encoding UTF8 -NoNewline }
}

# ------------------------------------------------------------------- secret guard

$script:SecretPatterns = @(
    '-----BEGIN [A-Z ]*PRIVATE KEY',
    'sk-[A-Za-z0-9]{24,}',
    'gh[pousr]_[A-Za-z0-9]{30,}',
    'xox[baprs]-[A-Za-z0-9-]{10,}',
    'AIza[0-9A-Za-z_\-]{30,}',
    '(?i)password\s*[:=]\s*["''][^"'']{6,}["'']'
)

# Only text files are scanned: a compiled firmware image legitimately embeds the
# mbedTLS PEM type strings ("-----BEGIN RSA PRIVATE KEY" and friends), which
# says nothing about a key being present.
$script:TextExtensions = @(
    '.c', '.cpp', '.h', '.hpp', '.md', '.txt', '.py', '.ps1', '.cmd', '.bat',
    '.yml', '.yaml', '.json', '.csv', '.lock', '.defaults', '.gitignore',
    '.gitattributes', '.svg', '.cmake'
)

function Test-StagedTextFiles {
    $names = (Invoke-Git -Arguments @('diff', '--cached', '--name-only')).Output -split "`n"

    foreach ($name in $names) {
        $name = $name.Trim()
        if ($name -eq '') { continue }

        $leaf = [System.IO.Path]::GetFileName($name)
        $ext  = [System.IO.Path]::GetExtension($name).ToLowerInvariant()
        $isText = ($script:TextExtensions -contains $ext) -or ($leaf -eq 'VERSION') -or ($leaf -eq 'SHA256SUMS')
        if (-not $isText) { continue }

        $path = Join-Path $RepoPath ($name -replace '/', '\')
        if (-not (Test-Path $path)) { continue }

        $content = Get-Content -Path $path -Raw -Encoding UTF8
        foreach ($pattern in $script:SecretPatterns) {
            if ($content -match $pattern) { return "$name matches $pattern" }
        }
    }
    return $null
}

function Restore-Repo {
    # A cycle that aborts after touching files must leave the tree exactly as it
    # found it, otherwise every later cycle refuses to run ("uncommitted
    # changes") and the publisher wedges itself.
    if ($DryRun) { return }

    Invoke-Git -Arguments @('reset', '--quiet') -AllowFailure | Out-Null
    Invoke-Git -Arguments @('checkout', '--', '.') -AllowFailure | Out-Null
    Invoke-Git -Arguments @('clean', '-fdq', '--', 'main', 'components', 'tools', 'firmware', 'CHANGELOG.md', 'VERSION') -AllowFailure | Out-Null
    Write-Log 'working tree restored after an aborted cycle' 'warn'
}

# --------------------------------------------------------------------------- main

$mutex = New-Object System.Threading.Mutex($false, 'Global\watch-esp32-publisher')
if (-not $mutex.WaitOne(0)) {
    Write-Log 'another cycle is already running, skipping' 'warn'
    return
}

try {
    Initialize-Git

    $script:RepoSlug = $null
    $originUrl = (Invoke-Git -Arguments @('remote', 'get-url', 'origin') -AllowFailure).Output.Trim()
    if ($originUrl -match 'github\.com[:/]+([^/\s]+)/([^/\s]+?)(?:\.git)?$') {
        $script:RepoSlug = '{0}/{1}' -f $Matches[1], $Matches[2]
    }

    if (-not (Test-Path (Join-Path $RepoPath '.git'))) { throw "not a git working tree: $RepoPath" }
    if (-not (Test-Path $ProjectPath)) { throw "project not found: $ProjectPath" }

    $state = Read-State

    # the repository must be clean: manual edits are committed by hand, and a
    # dirty tree means someone is working in it right now
    $status = (Invoke-Git -Arguments @('status', '--porcelain')).Output.Trim()
    if ($status -ne '') {
        Write-Log 'repository has uncommitted changes, skipping this cycle' 'warn'
        return
    }

    if ($state.pendingPush -eq $true) {
        Write-Log 'a previous commit is still unpushed, retrying the push' 'warn'
        $push = Invoke-Git -Arguments @('push', 'origin', 'main') -AllowFailure
        if ($push.Code -eq 0) {
            $state.pendingPush = $false
            $state.lastMode = 'push-retry'
            Save-State $state
            Write-Log 'push retry succeeded'
        } else {
            Write-Log ("push retry failed: {0}" -f $push.Output) 'error'
        }
        return
    }

    $entries = Get-MirrorEntries
    $fingerprint = Get-Fingerprint $entries

    if ($fingerprint -eq $state.fingerprint -and -not $Force) {
        return
    }

    $newest = Get-NewestWriteTime $entries
    $settled = ((Get-Date).ToUniversalTime() - $newest).TotalMinutes
    if ($settled -lt $SettleMinutes -and -not $Force) {
        Write-Log ("change detected but only {0:N1} min old, waiting" -f $settled)
        return
    }

    $repoVersion = (Get-Content (Join-Path $RepoPath 'VERSION') -Raw).Trim()
    $parts = $repoVersion.Split('.')
    $versions = @(Get-SessionVersions)
    $sessionTop = $versions | Sort-Object Major, Minor | Select-Object -Last 1
    $image = Get-LatestBackupImage

    $mode = 'none'
    if ($null -ne $sessionTop -and $null -ne $image) {
        if ($sessionTop.Major -gt [int]$parts[0] -or
            ($sessionTop.Major -eq [int]$parts[0] -and $sessionTop.Minor -gt [int]$parts[1])) {
            $mode = 'version'
        }
    }
    if ($mode -eq 'none') { $mode = 'snapshot' }

    Write-Log ("cycle: mode={0} repo={1} session={2} fingerprint={3}" -f `
        $mode, $repoVersion, $(if ($sessionTop) { "v$($sessionTop.Major).$($sessionTop.Minor)" } else { 'none' }), $fingerprint.Substring(0, 12))

    # fast-forward if origin moved (another machine, or a manual push)
    $fetch = Invoke-Git -Arguments @('fetch', '--quiet', 'origin') -AllowFailure
    if ($fetch.Code -eq 0) {
        $behind = (Invoke-Git -Arguments @('rev-list', '--count', 'HEAD..origin/main')).Output.Trim()
        if ($behind -ne '0' -and $behind -ne '') {
            $merge = Invoke-Git -Arguments @('merge', '--ff-only', 'origin/main') -AllowFailure
            if ($merge.Code -ne 0) {
                Write-Log 'origin/main has diverged; refusing to publish' 'error'
                return
            }
            Write-Log "fast-forwarded to origin/main ($behind commit(s))"
        }
    }

    $changed = @(Sync-Mirror $entries)

    if ($mode -eq 'snapshot' -and $changed.Count -eq 0) {
        # The fingerprint moved (a line ending, VERSIONS.md, an image) but no
        # published file differs: never rewrite the CHANGELOG for nothing.
        $state.fingerprint = $fingerprint
        Save-State $state
        Write-Log 'nothing to publish, state updated'
        return
    }

    $firmwareFolder = $null
    if ($mode -eq 'version') { $firmwareFolder = Copy-FirmwareImage $image }

    if ($mode -eq 'version') {
        $number = '{0}.{1}.0' -f $sessionTop.Major, $sessionTop.Minor
        Update-Changelog -Mode 'version' -Version $sessionTop -Image $image -Changed $changed
        if (-not $DryRun) { Set-Content -Path (Join-Path $RepoPath 'VERSION') -Value "$number`n" -Encoding ascii -NoNewline }
    } else {
        Update-Changelog -Mode 'snapshot' -Changed $changed
    }

    if ($DryRun) {
        Write-Log ("dry run: would publish {0}, {1} file(s) changed" -f $mode, $changed.Count)
        foreach ($file in $changed) { Write-Log "  $file" }
        return
    }

    Invoke-Git -Arguments @('add', '-A') | Out-Null

    $staged = (Invoke-Git -Arguments @('diff', '--cached', '--name-only')).Output.Trim()
    if ($staged -eq '') {
        $state.fingerprint = $fingerprint
        Save-State $state
        Write-Log 'no file change after all, state updated'
        return
    }

    $leak = Test-StagedTextFiles
    if ($null -ne $leak) {
        Restore-Repo
        Write-Log "REFUSING to publish: $leak" 'error'
        return
    }

    if ($mode -eq 'version') {
        $number = '{0}.{1}.0' -f $sessionTop.Major, $sessionTop.Minor
        $message = "v$number - $($sessionTop.Title)"
        $body = @"
Published automatically from the development session.

Application: $('{0:N0}' -f $image.Size) bytes
SHA-256: $($image.Hash)
Flashed as firmware/$firmwareFolder/montre.bin at offset 0x200000.
"@
        Invoke-Git -Arguments @('commit', '-q', '-m', $message, '-m', $body) | Out-Null
        Invoke-Git -Arguments @('tag', '-a', "v$number", '-m', "v$number - $($sessionTop.Title)") | Out-Null

        $push = Invoke-Git -Arguments @('push', 'origin', 'main') -AllowFailure
        if ($push.Code -ne 0) {
            $state.pendingPush = $true
            Save-State $state
            Write-Log "commit created but push failed: $($push.Output)" 'error'
            return
        }
        Invoke-Git -Arguments @('push', 'origin', "v$number") -AllowFailure | Out-Null

        $state.fingerprint = $fingerprint
        $state.version = $number
        $state.lastMode = 'version'
        Save-State $state
        Write-Log "published v$number ($($changed.Count) file(s), $($image.Size) bytes)"

        # release notes, best effort: a missing gh must not fail the publish,
        # and a clone that is not the canonical repository must never publish
        # a release onto it
        $gh = Get-Command gh -ErrorAction SilentlyContinue
        if ($gh -and $script:RepoSlug -eq 'withinaz/watch-esp32') {
            $notes = Join-Path $env:TEMP "watch-esp32-$number.md"
            $releaseBody = @"
Automatically published from the development session.

$($sessionTop.Body)

## Firmware

| | |
|---|---|
| Application | $('{0:N0}' -f $image.Size) bytes |
| SHA-256 | ``$($image.Hash)`` |
| Write offset | ``0x200000`` |

```powershell
cd firmware
.\flash.ps1
```

Full notes: [CHANGELOG]($RepoUrl/blob/main/CHANGELOG.md).
"@
            Set-Content -Path $notes -Value $releaseBody -Encoding UTF8
            $release = & $gh.Source release create "v$number" `
                (Join-Path $RepoPath "firmware\$firmwareFolder\montre.bin") `
                --repo withinaz/watch-esp32 --title "v$number - $($sessionTop.Title)" --notes-file $notes 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Log "release creation failed: $($release -join ' ')" 'warn'
            } else {
                Write-Log "release v$number created"
            }
            Remove-Item $notes -Force -ErrorAction SilentlyContinue
        }
    } else {
        $message = 'snapshot: {0} file(s) from the ESP32-206 session' -f $changed.Count
        $body = ($changed | Select-Object -First 20) -join "`n"
        Invoke-Git -Arguments @('commit', '-q', '-m', $message, '-m', $body) | Out-Null

        $push = Invoke-Git -Arguments @('push', 'origin', 'main') -AllowFailure
        if ($push.Code -ne 0) {
            $state.pendingPush = $true
            Save-State $state
            Write-Log "commit created but push failed: $($push.Output)" 'error'
            return
        }

        $state.fingerprint = $fingerprint
        $state.lastMode = 'snapshot'
        Save-State $state
        Write-Log "published snapshot ($($changed.Count) file(s))"
    }
}
catch {
    Restore-Repo
    Write-Log ("cycle failed: {0}" -f $_.Exception.Message) 'error'
    if (-not $Quiet) { Write-Error $_ }
    exit 1
}
finally {
    $mutex.ReleaseMutex()
    $mutex.Dispose()
}
