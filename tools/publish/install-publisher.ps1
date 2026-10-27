<#
.SYNOPSIS
    Installs (or removes) the scheduled task that publishes the ESP32-206
    session into watch-esp32.

.DESCRIPTION
    Registers watch-esp32-publisher. By default it runs twice a day, at 09:00
    and 20:00 local time, as the current user, interactively, with a limited
    token. The script is idempotent and publishes nothing when nothing changed,
    so the schedule only sets how often a change can reach GitHub.

    -EveryMinutes keeps the old rapid polling mode for a working session.

    Windows PowerShell 5.1 runs the task on purpose: it lives in System32 and
    cannot move. A Store-installed pwsh is only reachable through a 0-byte
    execution alias from an unattended start.

.PARAMETER At
    Daily times, 24-hour HH:mm. Default 09:00 and 20:00.

.PARAMETER EveryMinutes
    Use a repeating interval instead of daily times. 0 (default) disables it.

.PARAMETER Remove
    Unregister the task instead of creating it.

.EXAMPLE
    .\install-publisher.ps1
    .\install-publisher.ps1 -At 08:30,21:00
    .\install-publisher.ps1 -EveryMinutes 5
    .\install-publisher.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string[]] $At = @('09:00', '20:00'),
    [int]      $EveryMinutes = 0,
    [switch]   $Remove
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
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}"' -f $script)

$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 10)

if ($EveryMinutes -gt 0) {
    # rapid polling during a working session; schtasks handles the "repeat
    # forever" case that New-ScheduledTaskTrigger cannot express cleanly
    $tr = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}"' -f $script
    & schtasks.exe /Create /TN $TaskName /TR ("powershell.exe " + $tr) /SC MINUTE /MO $EveryMinutes /RL LIMITED /IT /F | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "schtasks failed with exit code $LASTEXITCODE" }

    Write-Host ''
    Write-Host "task '$TaskName' registered: every $EveryMinutes min" -ForegroundColor Green
    Write-Host ("next run: {0}" -f (Get-ScheduledTaskInfo -TaskName $TaskName).NextRunTime)
    Write-Host "log: $env:LOCALAPPDATA\watch-esp32-publisher\publish.log"
    return
} else {
    $triggers = @()
    foreach ($time in $At) {
        if ($time -notmatch '^\d{1,2}:\d{2}$') { throw "invalid time '$time', expected HH:mm" }
        $parts = $time.Split(':')
        $when = (Get-Date).Date.AddHours([int]$parts[0]).AddMinutes([int]$parts[1])
        $triggers += New-ScheduledTaskTrigger -Daily -At $when
    }
    $trigger = $triggers
    $schedule = "daily at " + ($At -join ' and ')
}

$principal = New-ScheduledTaskPrincipal -UserId ("{0}\{1}" -f $env:USERDOMAIN, $env:USERNAME) `
    -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force | Out-Null

$registered = Get-ScheduledTask -TaskName $TaskName
$info = Get-ScheduledTaskInfo -TaskName $TaskName

Write-Host ''
Write-Host "task '$TaskName' registered: $schedule" -ForegroundColor Green
Write-Host ("next run: {0}" -f $info.NextRunTime)
Write-Host "log: $env:LOCALAPPDATA\watch-esp32-publisher\publish.log"
Write-Host ''
$registered.Triggers | ForEach-Object {
    $at = $_.StartBoundary
    Write-Host ("  trigger: {0} at {1}" -f $_.CimClass.CimClassName, ($at -join ", "))
}
