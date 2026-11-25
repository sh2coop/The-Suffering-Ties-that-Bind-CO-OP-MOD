# Starts the game in every level for a short while with S2_TEST_CENSUS and gathers the "census:" lines (the testable
# method table) into runs\census.txt.   powershell -File tools\classrun.ps1 [-Seconds 24] [-From 0] [-Count 18]
param([int]$Seconds = 24, [int]$From = 0, [int]$Count = 18)
$proj = Split-Path -Parent $PSScriptRoot
$levels = @("00_Prison_Riot\00a_Prison_Riot", "01_Harbor\01a_Harbor_Base", "02_Streets_North\02a_Tunnels_to_Apt",
    "02_Streets_North\02b_Apartment", "03_Streets_South\03a_Streets", "03_Streets_South\03b_Streets",
    "03_Streets_South\03c_Streets", "02_Streets_North\02c_Tunnels_to_Harbor", "01_Harbor\01b_Harbor_Return",
    "04_Streets_East\04a_Truck_Chase", "05_Prison_Admissions\05_Prison_Admissions",
    "05_Prison_Admissions\05b_Prison_Kitchen", "06_Prison_Cellblock\06_Cellblock", "07_Prison_Industry\07a_Industry",
    "07_Prison_Industry\07b_Industry", "08_Drowningpool\08_Drowningpool", "09_Slums_Grouphome\09A_Slums_Grouphome",
    "09_Slums_Grouphome\09B_Slums_Grouphome")
$out = Join-Path $proj "runs\census.txt"
for ($i = $From; $i -lt [Math]::Min($From + $Count, $levels.Count); $i++) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run.ps1") -Seconds $Seconds -Level $levels[$i] -Name "census_run" -Vars "S2_TEST_CENSUS=800" -Tail 0 2>$null | Out-Null
    if ($LASTEXITCODE -eq 3) { "the game is running and no test started it"; break }
    "== " + $levels[$i] | Add-Content $out
    $log = Join-Path $proj "runs\census_run\log.txt"
    if (Test-Path $log) { Get-Content $log | Where-Object { $_ -match 'census: ' } | ForEach-Object { ($_ -replace '^.*census: ', '') } | Add-Content $out }
}
