<#
.SYNOPSIS
    Installs (or removes) the scheduled task that publishes the ESP32-206
    session into watch-esp32.

.DESCRIPTION
    Registers watch-esp32-publisher: every few minutes, run publish-session.ps1
    as the current user, interactively, with a limited token. The script is
    idempotent and does nothing when nothing changed, so a short period costs
    nothing.

    Windows PowerShell 5.1 is used on purpose: it lives in System32 and cannot
    move. A Store-installed pwsh is only reachable through a 0-byte execution
    alias from an unattended start.

.PARAMETER EveryMinutes
    Period of the task. Default 5.

.PARAMETER Remove
    Unregister the task instead of creating it.

.EXAMPLE
    .\install-publisher.ps1
    .\install-publisher.ps1 -EveryMinutes 15
    .\install-publisher.ps1 -Remove
#>
[CmdletBinding()]
param(
    [int]    $EveryMinutes = 5,
    [switch] $Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$TaskName = 'watch-esp32-publisher'

if ($Remove) {
    & schtasks.exe /Delete /TN $TaskName /F | Out-Null
    Write-Host "task '$TaskName' removed" -ForegroundColor Yellow
    return
}

$script = (Resolve-Path (Join-Path $PSScriptRoot 'publish-session.ps1')).Path
$tr = 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $script + '"'

& schtasks.exe /Create /TN $TaskName /TR $tr /SC MINUTE /MO $EveryMinutes /RL LIMITED /IT /F | Out-Null
if ($LASTEXITCODE -ne 0) { throw "schtasks failed with exit code $LASTEXITCODE" }

$query = & schtasks.exe /Query /TN $TaskName /FO LIST /V
$query | Select-String -Pattern 'Statut|Status|Prochaine|Next Run|R.sultat|Result|R.p.ter|Repeat' |
    ForEach-Object { $_.Line.Trim() }

Write-Host ''
Write-Host "task '$TaskName' registered: publish-session.ps1 every $EveryMinutes min" -ForegroundColor Green
Write-Host "log: $env:LOCALAPPDATA\watch-esp32-publisher\publish.log"
