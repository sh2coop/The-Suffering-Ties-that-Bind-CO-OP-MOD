# Test: a second copy of the game that joins a host on this PC (online co-op with two games on one PC).
# Start the host first (tools\run.ps1 -Vars "S2_COOP_HOST=1" ...), then:
#   powershell -File tools\join.ps1 -Seconds 40 -Script scripts\p2_walk.txt -Shot 30 -Name join1
# With -Address "" the copy does not join by itself: its controller script (and -Vars "S2_KEYS=...", keyboard keys)
# goes through the menu, as a player does: Options, Display and Co-op, Join online, the host's address.
# The copy runs from the same folder: it leaves the cmd file alone, plays its own controller script
# (pad_script_join.txt) and writes its own log. Its window is saved at -Shot seconds (runs\<Name>_window.png).
param(
    [int]$Seconds = 40,
    [string]$Address = "127.0.0.1",
    [string]$Script = "",
    [double]$Shot = 0,
    [string]$Name = "join",
    [string]$Vars = ""      # extra environment variables for this copy: "NAME=value;NAME2=value"

)
$proj = Split-Path -Parent $PSScriptRoot
$rel = Join-Path $proj "out\build\release"
$logdir = Join-Path $rel "logs"
$logsBefore = @(Get-ChildItem $logdir -File -Filter "suffering2*.log" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
if ($Script -ne "") { Copy-Item $Script (Join-Path $rel "pad_script_join.txt") -Force } else { Set-Content (Join-Path $rel "pad_script_join.txt") "" }
$env:S2_COOP_JOIN = $Address
$env:S2_PAD_SCRIPT = "pad_script_join.txt"
$env:S2_NO_CMD = "1"
# its frame as it shows it (the host's picture), saved to runs\<Name>_frame.bmp
$cap = Join-Path $rel "cap_join"
if (Test-Path $cap) { Get-ChildItem $cap | Remove-Item -Recurse -Force } else { New-Item -ItemType Directory -Force $cap | Out-Null }
if ($Shot -gt 0) { $env:S2_CAPTURE_AT = "$Shot"; $env:XBE_CAPTURE_DIR = $cap }
# the picture it shows 20 s after joining (the host's, as handed to its renderer): runs\<Name>_shown.bmp
$env:S2_JOIN_DUMP = Join-Path $proj ("runs\" + $Name + "_shown.bmp")
$set = @()
foreach ($pair in ($Vars -split ";" | Where-Object { $_ -match "=" })) {
    $k, $v = $pair -split "=", 2
    Set-Item -Path ("Env:" + $k) -Value $v
    $set += $k
}
$p = Start-Process (Join-Path $rel "Suffering2.exe") -WorkingDirectory $rel -PassThru
Remove-Item Env:S2_COOP_JOIN, Env:S2_PAD_SCRIPT, Env:S2_NO_CMD -ErrorAction SilentlyContinue
foreach ($k in $set) { Remove-Item ("Env:" + $k) -ErrorAction SilentlyContinue }
Remove-Item Env:S2_CAPTURE_AT, Env:XBE_CAPTURE_DIR, Env:S2_JOIN_DUMP -ErrorAction SilentlyContinue
"joining copy: process $($p.Id)"
$start = Get-Date
$shot = $Shot -gt 0
while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    if ($shot -and ((Get-Date) - $start).TotalSeconds -ge $Shot) {
        $shot = $false
        python (Join-Path $PSScriptRoot "winshot.py") (Join-Path $proj ("runs\" + $Name + "_window.png")) 0 $p.Id
    }
    Start-Sleep -Milliseconds 250
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "joining copy stopped after $Seconds s" } else { "joining copy ended by itself (exit $($p.ExitCode))" }
Start-Sleep -Milliseconds 500
$log = Get-ChildItem $logdir -File -Filter "suffering2*.log" -ErrorAction SilentlyContinue | Where-Object { $logsBefore -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
foreach ($c in Get-ChildItem $cap -Directory -ErrorAction SilentlyContinue) {
    $f = Join-Path $c.FullName "final.bmp"
    if (Test-Path $f) { Copy-Item $f (Join-Path $proj ("runs\" + $Name + "_frame.bmp")) -Force; "frame: runs\" + $Name + "_frame.bmp" }
}
if ($log) { Copy-Item $log.FullName (Join-Path $proj ("runs\" + $Name + "_log.txt")) -Force; "log: runs\" + $Name + "_log.txt" }
