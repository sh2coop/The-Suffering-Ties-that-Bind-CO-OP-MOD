# Smoke test of the levels: starts the game in each one (the game's own start-level option, see src\game\diag.cpp),
# lets it run with a pad script, captures frames, and reports per level: did it load, crash, log an error or an
# assert, and the slowest second. Pictures and logs go to runs\levels_<name>; a sheet of all captures to
# runs\levels_sheet.png.
#   powershell -File tools\levels.ps1 [-Seconds 45] [-Only "01_Harbor\01a_Harbor_Base,..."]
param(
    [int]$Seconds = 45,
    [string]$Only = "",
    [string]$Captures = "14,24,34,42"
)
$proj = Split-Path -Parent $PSScriptRoot
$rel = Join-Path $proj "out\build\release"
$levels = @(
    "00_Prison_Riot\00a_Prison_Riot", "01_Harbor\01a_Harbor_Base", "02_Streets_North\02a_Tunnels_to_Apt",
    "02_Streets_North\02b_Apartment", "03_Streets_South\03a_Streets", "03_Streets_South\03b_Streets",
    "03_Streets_South\03c_Streets", "02_Streets_North\02c_Tunnels_to_Harbor", "01_Harbor\01b_Harbor_Return",
    "04_Streets_East\04a_Truck_Chase", "05_Prison_Admissions\05_Prison_Admissions",
    "05_Prison_Admissions\05b_Prison_Kitchen", "06_Prison_Cellblock\06_Cellblock", "07_Prison_Industry\07a_Industry",
    "07_Prison_Industry\07b_Industry", "08_Drowningpool\08_Drowningpool", "09_Slums_Grouphome\09A_Slums_Grouphome",
    "09_Slums_Grouphome\09B_Slums_Grouphome")
if ($Only -ne "") { $levels = @($Only -split ",") }
$script = Join-Path $proj "scripts\level_smoke.txt"
$report = @()
foreach ($level in $levels) {
    $short = ($level -split "\\")[-1]
    $name = "levels_" + $short
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run.ps1") -Seconds $Seconds -FpsCap 60 -VSync `
        -Level $level -Script $script -Captures $Captures -Name $name -Tail 0 2>$null
    if ($LASTEXITCODE -eq 3) { "the game is running and no test started it: stopping the smoke test"; break }
    $state = if ($out -match "still running") { "ran $Seconds s" } else { ($out | Where-Object { $_ -match "exited" } | Select-Object -First 1) }
    $logPath = Join-Path $proj ("runs\" + $name + "\log.txt")   # run.ps1 copies this run's log there
    if (-not (Test-Path $logPath)) { $line = "{0,-24} made no log" -f $short; $line; $report += $line; continue }
    $text = Get-Content $logPath
    $loaded = @($text | Where-Object { $_ -match [regex]::Escape($short + ".lvl") }).Count -gt 0
    $errors = @($text | Where-Object { $_ -match "\[error\]|game assert|guest trap|exception C|freeze report: no frame" })
    $notes = @($text | Where-Object { $_ -match "not run|dropped|unknown handle|unimplemented|not applied|\[warn\]" } | ForEach-Object { ($_ -replace '^\[[^\]]*\] \[[^\]]*\] \[[^\]]*\] ', '') } | Sort-Object -Unique)
    $fps = @($text | ForEach-Object { if ($_ -match '\[\s*([0-9.]+)\].*perf: ([0-9.]+) fps') { if ([double]$matches[1] -gt 12) { [double]$matches[2] } } })
    $min = if ($fps.Count) { ($fps | Measure-Object -Minimum).Minimum } else { 0 }
    $slow = @($fps | Where-Object { $_ -lt 59 }).Count
    $line = "{0,-24} {1,-10} loaded {2,-5} errors {3,2}  seconds under 59 fps {4,2} of {5,2} (slowest {6:N1})" -f $short, $state, $loaded, $errors.Count, $slow, $fps.Count, $min
    $line
    $report += $line
    foreach ($e in ($errors | Select-Object -First 4)) { $t = "      " + $e.Substring(0, [Math]::Min(190, $e.Length)); $t; $report += $t }
    foreach ($n in ($notes | Select-Object -First 5)) { $t = "      note: " + $n.Substring(0, [Math]::Min(180, $n.Length)); $t; $report += $t }
}
Set-Content (Join-Path $proj "runs\levels_report.txt") $report
