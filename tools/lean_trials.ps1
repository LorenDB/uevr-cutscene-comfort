# Repeated lean measurement, controlled for scene animation.
#
# A moving scene changes between any two captures, so a single before and
# after cannot tell a viewpoint translation apart from the scene simply
# animating. Each repetition here takes two captures with no change between
# them and two captures with a translation between them, using the same delay
# both times. Animation therefore contributes equally to both and the
# difference between the two series is what the translation added.

param(
    [int]$Reps = 6,
    [string]$Title = 'SpongeBob SquarePants: Battle for Bikini Bottom - Rehydrated',
    [int]$Offset = 40,
    [int]$GapMs = 900,
    [string]$Scratch = "$env:TEMP\lean_trials"
)

$ErrorActionPreference = 'Continue'
$tools = Split-Path $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Scratch | Out-Null

function Cap($name) {
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'lean_capture.ps1') `
        -OutPath (Join-Path $Scratch "$name.png") -Title $Title | Out-Null
}

function SetOff($v) {
    $b = @{ key = 'VR_CameraRightOffset'; value = "$v" } | ConvertTo-Json -Compress
    try { Invoke-RestMethod -Uri 'http://127.0.0.1:8899/api/vr/settings' -Method Post -ContentType 'application/json' -Body $b -TimeoutSec 10 | Out-Null } catch { }
}

function SceneDiff($a, $b) {
    $out = powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'lean_compare.ps1') `
        -A (Join-Path $Scratch "$a.png") -B (Join-Path $Scratch "$b.png")
    $scene = 0.0; $mask = 0.0
    foreach ($line in $out) {
        if ($line -match 'scene region\s+meanAbsDiff = ([\d.]+)') { $scene = [double]$Matches[1] }
        if ($line -match 'mask  region\s+meanAbsDiff = ([\d.]+)') { $mask = [double]$Matches[1] }
    }
    return @($scene, $mask)
}

$control = @(); $test = @(); $maskAll = @()

for ($i = 1; $i -le $Reps; $i++) {
    # Control: same gap, no translation.
    SetOff 0
    Start-Sleep -Milliseconds $GapMs
    Cap "c${i}_a"
    Start-Sleep -Milliseconds $GapMs
    Cap "c${i}_b"

    # Test: same gap, translation applied in between.
    Start-Sleep -Milliseconds $GapMs
    Cap "t${i}_a"
    SetOff $Offset
    Start-Sleep -Milliseconds $GapMs
    Cap "t${i}_b"
    SetOff 0

    $c = SceneDiff "c${i}_a" "c${i}_b"
    $t = SceneDiff "t${i}_a" "t${i}_b"
    $control += $c[0]; $test += $t[0]
    $maskAll += $c[1]; $maskAll += $t[1]
    "rep $i   control $($c[0])   test $($t[0])   mask $($c[1])/$($t[1])"
}

$cAvg = [Math]::Round(($control | Measure-Object -Average).Average, 3)
$tAvg = [Math]::Round(($test | Measure-Object -Average).Average, 3)
$mMax = ($maskAll | Measure-Object -Maximum).Maximum
$wins = 0
for ($i = 0; $i -lt $Reps; $i++) { if ($test[$i] -gt $control[$i]) { $wins++ } }

''
'=== SUMMARY ==='
"control mean scene diff (no translation) = $cAvg"
"test    mean scene diff (translation)    = $tAvg"
"test exceeded control in $wins of $Reps reps"
"largest mask diff seen anywhere          = $mMax"
