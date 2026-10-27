<#
.SYNOPSIS
    Flash a prebuilt watch-esp32 firmware onto the board.

.DESCRIPTION
    Verifies the SHA-256 of every image against SHA256SUMS before writing
    anything, then calls esptool.

    By default only the application is written, at 0x200000. The nvs
    partition (0x3B000) is left untouched, so the WiFi credentials already
    stored on the watch survive.

.EXAMPLE
    .\flash.ps1
    .\flash.ps1 -Port COM7
    .\flash.ps1 -Full          # bootloader + partition table + application
    .\flash.ps1 -DryRun        # verify the files, do not touch the board

.NOTES
    Board: Waveshare ESP32-S3-Touch-AMOLED-2.06 (native USB-Serial/JTAG).
#>
[CmdletBinding()]
param(
    [string] $Port = 'auto',
    [string] $Version = 'v3.5',
    [switch] $Full,
    [switch] $DryRun,
    [int]    $Baud = 921600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$dir  = Join-Path $root $Version
if (-not (Test-Path $dir)) {
    throw "version directory not found: $dir"
}

$sumsFile = Join-Path $dir 'SHA256SUMS'
if (-not (Test-Path $sumsFile)) {
    throw "SHA256SUMS not found in $dir"
}

function Get-ExpectedHash {
    param([string] $Name)

    foreach ($line in Get-Content $sumsFile) {
        if ($line -match '^\s*([0-9a-fA-F]{64})\s+\*?(.+?)\s*$') {
            if ($Matches[2] -eq $Name) { return $Matches[1].ToLowerInvariant() }
        }
    }
    return $null
}

$images = [System.Collections.Generic.List[object]]::new()
if ($Full) {
    $images.Add([pscustomobject]@{ Offset = '0x0';      Name = 'bootloader.bin';      Optional = $false; Path = '' })
    $images.Add([pscustomobject]@{ Offset = '0x8000';   Name = 'partition-table.bin'; Optional = $false; Path = '' })
}
$images.Add([pscustomobject]@{ Offset = '0x200000'; Name = 'montre.bin';         Optional = $false; Path = '' })

Write-Host ''
Write-Host "watch-esp32 -- firmware $Version" -ForegroundColor Cyan
Write-Host ''

foreach ($img in $images) {
    $path = Join-Path $dir $img.Name
    if (-not (Test-Path $path)) { throw "missing image: $path" }

    $actual   = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = Get-ExpectedHash $img.Name
    if (-not $expected) { throw "no checksum listed for $($img.Name) in SHA256SUMS" }

    if ($actual -ne $expected) {
        throw "$($img.Name): SHA-256 mismatch`n  expected $expected`n  actual   $actual"
    }

    $size = (Get-Item $path).Length
    Write-Host ("  {0,-22} {1,10:N0} bytes  @ {2,-8} sha256 {3}" -f `
        $img.Name, $size, $img.Offset, $actual.Substring(0, 16) + '...') -ForegroundColor Green
    $img.Path = $path
}

Write-Host ''
if ($DryRun) {
    Write-Host 'Dry run: images verified, nothing was written.' -ForegroundColor Yellow
    return
}

function Resolve-Esptool {
    try {
        $null = & python -m esptool version 2>&1
        if ($LASTEXITCODE -eq 0) { return @('python', '-m', 'esptool') }
    } catch { }

    # ESP-IDF's own python environment, when the toolchain is installed.
    $idfPy = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
    if (Test-Path $idfPy) { return @($idfPy, '-m', 'esptool') }

    throw 'esptool not found: install it with "pip install esptool" or use the ESP-IDF environment'
}

$esptool = Resolve-Esptool

$esptoolArgs = @('--chip', 'esp32s3')
if ($Port -ne 'auto') { $esptoolArgs += @('-p', $Port) }
$esptoolArgs += @('-b', "$Baud", '--before', 'default_reset', '--after', 'hard_reset',
           'write_flash', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '32MB')

foreach ($img in $images) {
    $esptoolArgs += @($img.Offset, $img.Path)
}

Write-Host ("Flashing with: {0} {1}" -f $esptool[0], ($esptoolArgs -join ' ')) -ForegroundColor DarkGray
Write-Host ''

$exe  = $esptool[0]
$rest = @($esptool[1..($esptool.Count - 1)]) + $esptoolArgs
& $exe @rest
if ($LASTEXITCODE -ne 0) { throw "esptool failed with exit code $LASTEXITCODE" }

Write-Host ''
Write-Host 'Flashed. Expected boot log:' -ForegroundColor Cyan
Write-Host '  MONTRE: ecran 410x502 pret'
Write-Host '  MONTRE: === montre : interface prete ==='
Write-Host '  Brookesia: Library version: 0.5.0'
Write-Host '  MONTRE: WiFi : connecte, IP ...'
Write-Host ''
Write-Host 'If the board does not restart: unplug it, hold BOOT, replug, flash again.' -ForegroundColor DarkGray

