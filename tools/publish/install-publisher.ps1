<#
.SYNOPSIS
    Installs (or removes) the scheduled task that publishes the ESP32-206
    session into watch-esp32.

.DESCRIPTION
    Registers watch-esp32-publisher to run EVERY HOUR as the current user,
    interactively, with a limited token.

    Running hourly does not mean publishing hourly. The script only publishes
    during its slots (09:00 and 20:00 by default) and, at any other hour, only
    when a previous cycle failed. So the task is also the retry loop: a failed
    publication is attempted again every hour, for as long as it takes, until a
    cycle completes.

    Windows PowerShell 5.1 runs the task on purpose: it lives in System32 and
    cannot move. A Store-installed pwsh is only reachable through a 0-byte
    execution alias from an unattended start.

.PARAMETER SlotHours
    Hours of the day at which a cycle is allowed to publish. Default 9 and 20.
    Passed to the script in the task action.

.PARAMETER Rapid
    Poll every five minutes instead, for a session where each change should
    land immediately. Bypasses the slot logic (every run is due).

.PARAMETER Remove
    Unregister the task instead of creating it.

.EXAMPLE
    .\install-publisher.ps1
    .\install-publisher.ps1 -SlotHours 8,21
    .\install-publisher.ps1 -Rapid
    .\install-publisher.ps1 -Remove
#>
[CmdletBinding()]
param(
    [int[]]  $SlotHours = @(9, 20),
    [switch] $Rapid,
    [switch] $Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$TaskName = 'watch-esp32-publisher'

if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    & schtasks.exe /Delete /TN $TaskName /F 2>$null | Out-Null
    Write-Host "task '$TaskName' removed" -ForegroundColor Yellow
    return
}

$script = (Resolve-Path (Join-Path $PSScriptRoot 'publish-session.ps1')).Path

if ($Rapid) {
    $tr = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Force' -f $script
    & schtasks.exe /Create /TN $TaskName /TR ('powershell.exe ' + $tr) /SC MINUTE /MO 5 /RL LIMITED /IT /F | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "schtasks failed with exit code $LASTEXITCODE" }
    $schedule = 'every 5 min (rapid mode)'
} else {
    $slots = ($SlotHours | Sort-Object) -join ','
    $tr = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -SlotHours {1}' -f $script, $slots
    & schtasks.exe /Create /TN $TaskName /TR ('powershell.exe ' + $tr) /SC HOURLY /MO 1 /RL LIMITED /IT /F | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "schtasks failed with exit code $LASTEXITCODE" }
    $schedule = "hourly, publishing at $($SlotHours -join ':00, '):00"
}

# schtasks cannot express these; they matter more than the schedule
$task = Get-ScheduledTask -TaskName $TaskName
$task.Settings.StartWhenAvailable = $true
$task.Settings.DisallowStartIfOnBatteries = $false
$task.Settings.StopIfGoingOnBatteries = $false
$task.Settings.MultipleInstances = 'IgnoreNew'
$task.Settings.ExecutionTimeLimit = 'PT10M'
Set-ScheduledTask -TaskName $TaskName -Settings $task.Settings | Out-Null

$info = Get-ScheduledTaskInfo -TaskName $TaskName

Write-Host ''
Write-Host "task '$TaskName' registered: $schedule" -ForegroundColor Green
Write-Host ("next run: {0}" -f $info.NextRunTime)
Write-Host "log: $env:LOCALAPPDATA\watch-esp32-publisher\publish.log"
Write-Host ''
Write-Host '  a cycle publishes at the slot hours, and at any other hour only'
Write-Host '  to retry a cycle that did not complete.'
