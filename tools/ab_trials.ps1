# Paired A/B for the theater frame.
#
# One conclusion already died here from a single sample per arm, so this runs
# matched pairs: the same launch procedure, alternating only theater_frame,
# and it records how long each run kept serving. Anything that kills the game
# regardless of the setting shows up as both arms failing at the same rate.

param(
    [int]$Pairs = 6,
    [int]$WatchSeconds = 90
)

$ErrorActionPreference = 'Continue'

$exe      = 'E:\SteamLibrary\steamapps\common\Halo Campaign Evolved\Meteorite\Binaries\Win64\HaloCampaignEvolved.exe'
$uevrDir  = 'C:\Users\ellio\AppData\Local\Temp\claude\C--\66022472-f4f4-4486-9b30-a61d9803fd5d\scratchpad\uevr-official'
$injector = Join-Path $uevrDir 'UEVRInjector.exe'
$profile  = 'C:\Users\ellio\AppData\Roaming\UnrealVRMod\HaloCampaignEvolved'
$ini      = Join-Path $profile 'cutscene_comfort.ini'
$log      = Join-Path $profile 'log.txt'

function Stop-Game {
    Get-Process -Name HaloCampaignEvolved, UEVRInjector -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 6
}

function Set-Frame([int]$on) {
    @"
theater_frame=$on
simulate_cutscene=1
frame_width=0.62
frame_height=0.56
frame_feather=0.22
frame_convergence=0.012
frame_opacity=1
frame_fade_seconds=0.2
zero_offsets=1
show_ui=0
"@ | Set-Content $ini -Encoding ASCII
}

function Invoke-Trial([int]$frameOn) {
    Stop-Game
    Set-Frame $frameOn

    Start-Process -FilePath $exe -ArgumentList '-windowed' -WorkingDirectory (Split-Path $exe) | Out-Null

    # Wait for a main window before injecting, same as the manual runs.
    $deadline = (Get-Date).AddSeconds(120)
    $proc = $null
    while ((Get-Date) -lt $deadline) {
        $proc = Get-Process -Name HaloCampaignEvolved -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($proc -and $proc.MainWindowHandle -ne 0) { break }
        Start-Sleep -Seconds 2
    }
    if (-not $proc) { return [pscustomobject]@{ frame = $frameOn; result = 'no-launch'; seconds = 0 } }

    Start-Sleep -Seconds 10
    Start-Process -FilePath $injector -ArgumentList '--attach=HaloCampaignEvolved.exe' -WorkingDirectory $uevrDir -WindowStyle Hidden | Out-Null

    # Wait until the plugin reports it is running before starting the clock,
    # so every trial is timed from the same milestone.
    $deadline = (Get-Date).AddSeconds(180)
    $started = $false
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'cutscene started' -Quiet -ErrorAction SilentlyContinue)) {
            $started = $true; break
        }
        if (-not (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)) {
            return [pscustomobject]@{ frame = $frameOn; result = 'died-before-start'; seconds = 0 }
        }
        Start-Sleep -Seconds 3
    }
    if (-not $started) { return [pscustomobject]@{ frame = $frameOn; result = 'never-started'; seconds = 0 } }

    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $WatchSeconds) {
        Start-Sleep -Seconds 3
        try {
            $null = Invoke-RestMethod -Uri 'http://127.0.0.1:8899/api/game_info' -TimeoutSec 3
        } catch {
            return [pscustomobject]@{
                frame = $frameOn; result = 'died'
                seconds = [int]((Get-Date) - $t0).TotalSeconds
            }
        }
    }

    return [pscustomobject]@{ frame = $frameOn; result = 'survived'; seconds = $WatchSeconds }
}

$results = @()
for ($i = 1; $i -le $Pairs; $i++) {
    foreach ($on in 1, 0) {
        $r = Invoke-Trial $on
        $results += $r
        "pair $i  frame=$on  -> $($r.result) at $($r.seconds)s"
    }
}

Stop-Game

"`n=== SUMMARY ==="
foreach ($on in 1, 0) {
    $arm = $results | Where-Object { $_.frame -eq $on }
    $died = @($arm | Where-Object { $_.result -eq 'died' })
    $surv = @($arm | Where-Object { $_.result -eq 'survived' })
    $other = @($arm | Where-Object { $_.result -notin 'died', 'survived' })
    $label = if ($on -eq 1) { 'frame ON ' } else { 'frame OFF' }
    $avg = if ($died.Count) { [int](($died | Measure-Object seconds -Average).Average) } else { 0 }
    "$label  survived=$($surv.Count)  died=$($died.Count)  unusable=$($other.Count)  meanDeathSecs=$avg"
}
