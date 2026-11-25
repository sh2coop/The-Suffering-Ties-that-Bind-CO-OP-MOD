# Hunts the freeze: starts the game in a level again and again until a run stops drawing, then lists what every
# thread was doing, from the game's own freeze report (src\game\watchdog.cpp, written after 20 s without a frame).
#   powershell -File tools\hunt.ps1 [-Level 00_Prison_Riot\00a_Prison_Riot] [-Tries 12] [-Seconds 22]
param(
    [string]$Level = "00_Prison_Riot\00a_Prison_Riot",
    [int]$Tries = 12,
    [int]$Seconds = 36,
    [string]$Vars = ""
)
$proj = Split-Path -Parent $PSScriptRoot
$rel = Join-Path $proj "out\build\release"
for ($i = 1; $i -le $Tries; $i++) {
    $before = @(Get-ChildItem (Join-Path $rel "logs") -File -Filter "suffering2*.log" | ForEach-Object Name)
    $more = @()
    if ($Vars -ne "") { $more = @("-Vars", $Vars) }   # an empty value would not bind
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run.ps1") -Seconds $Seconds -Keep -FpsCap 60 -VSync -Level $Level -Tail 0 @more 2>&1
    if ($LASTEXITCODE -eq 3) { "the game is running and no test started it"; exit 3 }
    # this run's log is the one that was not there before (the runtime numbers them upwards)
    $log = Get-ChildItem (Join-Path $rel "logs") -File -Filter "suffering2*.log" | Where-Object { $before -notcontains $_.Name } | Sort-Object Name | Select-Object -Last 1
    if (-not $log) { "try ${i}: the run made no log"; $out | Select-Object -First 5; exit 2 }
    $text = Get-Content $log.FullName
    $frames = @($text | Where-Object { $_ -match 'perf: [0-9.]+ fps' }).Count
    $report = @($text | Where-Object { $_ -match 'freeze report' })
    "try {0}: {1} seconds with frames, {2}" -f $i, $frames, $(if ($report.Count) { "FROZEN" } else { "no freeze" })
    if ($report.Count) {
        "log: $($log.FullName)"
        $text | Where-Object { $_ -notmatch "perf:|freeze report" } | Select-Object -Last 5 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
        # the threads running game code or the renderer, one frame a line
        foreach ($r in ($report | Where-Object { $_ -match 'sub_[0-9A-F]{8}|render_thread|Threaded|D3D12Backend' })) {
            ""
            (($r -replace '^.*freeze report: ', '') -split ' < ') | Select-Object -First 45
        }
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "stop.ps1")
        exit 1
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "stop.ps1") | Out-Null
}
"no freeze in $Tries tries"
