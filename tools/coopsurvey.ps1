# Co-op in every level: starts the game in each level with split-screen co-op on and nobody at the controllers,
# lets it run, and reports per level: did Player 2 come in, how long the view was one (a scene), who was hurt by
# whom (the "coop: hits" lines, added up), errors, and where both players stood at the end.
#   powershell -File tools\coopsurvey.ps1 [-Seconds 75] [-From 0] [-Count 18]
# Logs and one capture per level go to runs\surv_<level>; the report is printed and added to runs\coopsurvey.txt.
param(
    [int]$Seconds = 75,
    [int]$From = 0,
    [int]$Count = 18
)
$proj = Split-Path -Parent $PSScriptRoot
$levels = @(
    "00_Prison_Riot\00a_Prison_Riot", "01_Harbor\01a_Harbor_Base", "02_Streets_North\02a_Tunnels_to_Apt",
    "02_Streets_North\02b_Apartment", "03_Streets_South\03a_Streets", "03_Streets_South\03b_Streets",
    "03_Streets_South\03c_Streets", "02_Streets_North\02c_Tunnels_to_Harbor", "01_Harbor\01b_Harbor_Return",
    "04_Streets_East\04a_Truck_Chase", "05_Prison_Admissions\05_Prison_Admissions",
    "05_Prison_Admissions\05b_Prison_Kitchen", "06_Prison_Cellblock\06_Cellblock", "07_Prison_Industry\07a_Industry",
    "07_Prison_Industry\07b_Industry", "08_Drowningpool\08_Drowningpool", "09_Slums_Grouphome\09A_Slums_Grouphome",
    "09_Slums_Grouphome\09B_Slums_Grouphome")
$report = Join-Path $proj "runs\coopsurvey.txt"
for ($i = $From; $i -lt [Math]::Min($From + $Count, $levels.Count); $i++) {
    $level = $levels[$i]
    $short = ($level -split "\\")[-1]
    $name = "surv_" + $short
    $cap = [string]($Seconds - 5)
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run.ps1") -Seconds $Seconds -FpsCap 60 `
        -Level $level -Captures $cap -Name $name -Vars "S2_COOP=1" -Tail 0 2>$null
    if ($LASTEXITCODE -eq 3) { "the game is running and no test started it: stopping the survey"; break }
    $logPath = Join-Path $proj ("runs\" + $name + "\log.txt")
    if (-not (Test-Path $logPath)) { $line = "{0,-22} made no log" -f $short; $line; Add-Content $report $line; continue }
    $text = Get-Content $logPath
    $ended = if ($out -match "still running") { "ran" } else { "ENDED EARLY" }
    $second = @($text | Where-Object { $_ -match "coop: spawning a second object" }).Count
    $errors = @($text | Where-Object { $_ -match "\[error\]|game assert|guest trap|exception C|freeze report: no frame" }).Count
    # seconds with one view (a scene), from the "coop: views" lines
    $one = 0.0; $since = -1.0; $last = 0.0
    foreach ($l in $text) {
        if ($l -match '^\[\s*([0-9.]+)\]') { $last = [double]$matches[1] }
        if ($l -match 'coop: views: one') { $since = $last }
        elseif ($l -match 'coop: views:' -and $since -ge 0) { $one += $last - $since; $since = -1.0 }
    }
    if ($since -ge 0) { $one += $last - $since }
    # who hurt whom, added up
    $hits = @{}
    foreach ($l in $text) {
        if ($l -match 'coop: hits: (.+): ([0-9.]+) in (\d+)$') {
            $k = $matches[1]; if (-not $hits.ContainsKey($k)) { $hits[$k] = 0.0 }; $hits[$k] += [double]$matches[2]
        }
    }
    $hitText = if ($hits.Count) { ($hits.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0} {1:N0}" -f $_.Name, $_.Value }) -join "; " } else { "nobody hurt" }
    $whereLine = $text | Where-Object { $_ -match "coop: player 1 at" } | Select-Object -Last 1
    $where = if ($whereLine) { ([string]$whereLine) -replace '^.*coop: ', '' } else { "no place logged" }
    $line = "{0,-22} {1,-11} Player 2 made {2}x, one view {3,5:N0} s of {4}, errors {5} | {6} | {7}" -f $short, $ended, $second, $one, $Seconds, $errors, $hitText, $where
    $line
    Add-Content $report $line
}
