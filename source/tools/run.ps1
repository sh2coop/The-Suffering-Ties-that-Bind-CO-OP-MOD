# Test run: starts the game, lets it run for -Seconds (or until it exits), takes frame captures at the given
# times, then stops it and prints the end of the log.
#   powershell -File tools\run.ps1 -Seconds 60 -Captures 20,40 -Name boot1
# Frame captures land in out\build\release\cap (the game writes them on a CAPTURE in the cmd file), and the
# log and captures of each run are copied to runs\<Name>.
param(
    [int]$Seconds = 30,
    [string]$Captures = "",   # seconds, comma separated: "5,10,20"
    [string]$Name = "",
    [switch]$Keep,          # leave the game running
    [string]$Script = "",   # a pad script file to copy to pad_script.txt (empty: no script)
    [string]$Script2 = "",  # a pad script file for a second controller on port 1 (empty: no second controller)
    [int]$Tail = 60,
    [double]$Every = 0,     # also capture every this many seconds, from -From to the end
    [double]$From = 0,
    [int]$FpsCap = 0,       # settings.toml fps_cap for this run (0 = no cap)
    [switch]$VSync,
    [switch]$Audio,         # record the game's sound to audio.wav in the run's folder and log its level each second
    [string]$Mode = "windowed",   # windowed, borderless or fullscreen
    [string]$Shots = "",    # seconds, comma separated: screenshots of the whole desktop into runs\<Name>
    [string]$Level = "",    # a level file under English\NTSC\Levels, such as 01_Harbor\01a_Harbor_Base: the game starts there, no menus
    [string]$Vars = "",     # extra environment variables for the game: "NAME=value;NAME2=value"
    [int]$Width = 1280,
    [int]$Height = 720,
    [switch]$DumpTextures   # each texture the game uses, once, into textures/dumps beside the exe
)
$proj = Split-Path -Parent $PSScriptRoot
$rel = Join-Path $proj "out\build\release"
# A test run only stops a game that an earlier test run started (its process id is in test.pid). A game the
# player started is left alone: the test does not run then.
$pidFile = Join-Path $rel "test.pid"
$mine = if (Test-Path $pidFile) { [int](Get-Content $pidFile) } else { 0 }
foreach ($g in Get-Process Suffering2 -ErrorAction SilentlyContinue) {
    if ($g.Id -eq $mine) { Stop-Process -Id $g.Id -Force }
    else { "The game is running and no test started it (process $($g.Id)): no test run."; exit 3 }
}
Start-Sleep -Milliseconds 500
if ($Script -ne "") { Copy-Item $Script (Join-Path $rel "pad_script.txt") -Force } else { Set-Content (Join-Path $rel "pad_script.txt") "" }
if ($Script2 -ne "") { Copy-Item $Script2 (Join-Path $rel "pad_script2.txt") -Force } else { Set-Content (Join-Path $rel "pad_script2.txt") "" }
Set-Content (Join-Path $rel "cmd") ""
$cap = Join-Path $rel "cap"
New-Item -ItemType Directory -Force $cap | Out-Null
Get-ChildItem $cap | Remove-Item -Recurse -Force
$logdir = Join-Path $rel "logs"
# logs are kept (the runtime keeps the newest 30): a run the player made may be the one to look at
# The runtime rebuilds Suffering2.dll itself when the exe is newer than the DLL (the release flow). In this build
# tree ninja relinks the DLL whenever it has to, so an older DLL is still the right one: mark it current.
$exe = Get-Item (Join-Path $rel "Suffering2.exe")
$dll = Get-Item (Join-Path $rel "Suffering2.dll")
if ($dll.LastWriteTime -le $exe.LastWriteTime) { $dll.LastWriteTime = $exe.LastWriteTime.AddSeconds(1) }
$vs = if ($VSync) { "true" } else { "false" }
# the player's own settings are put back after the run
$settings = Join-Path $rel "settings.toml"
$kept = Join-Path $rel "settings.player.toml"
if ((Test-Path $settings) -and -not (Test-Path $kept)) { Copy-Item $settings $kept -Force }
$size = if ($Mode -eq "windowed") { @("width = $Width", "height = $Height") } else { @() }
Set-Content $settings (@("display_mode = '$Mode'") + $size + @("vsync = $vs", "fps_cap = $FpsCap") + $(if ($DumpTextures) { @("dump_textures = true") } else { @() }))
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public class S2Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[S2Dpi]::SetProcessDPIAware() | Out-Null
$shotTimes = @($Shots -split "," | Where-Object { $_ -ne "" } | ForEach-Object { [double]$_ } | Sort-Object)
$shotDir = if ($Name -ne "") { Join-Path $proj ("runs\" + $Name) } else { $rel }
New-Item -ItemType Directory -Force $shotDir | Out-Null
$env:XBE_CAPTURE_DIR = $cap
$env:XBE_PERF = "1"
$env:S2_AUDIO_WAV = ""; $env:S2_AUDIO_LOG = ""
$env:S2_LEVEL = $Level
$set = @()
foreach ($pair in ($Vars -split ";" | Where-Object { $_ -match "=" })) {
    $k, $v = $pair -split "=", 2
    Set-Item -Path ("Env:" + $k) -Value $v
    $set += $k
}
if ($Audio) { $env:S2_AUDIO_WAV = (Join-Path $rel "audio.wav"); $env:S2_AUDIO_LOG = "1" }
# this run's log is the one that is not there yet (the runtime numbers them upwards and keeps 30)
$logsBefore = @(Get-ChildItem $logdir -File -Filter "suffering2*.log" -ErrorAction SilentlyContinue | ForEach-Object Name)
# test runs are not seen or heard: the window is off screen, never in front, silent, and the game runs at low priority
$env:XBE_TEST_HIDDEN = "1"
$p = Start-Process -FilePath (Join-Path $rel "Suffering2.exe") -WorkingDirectory $rel -PassThru
try { $p.PriorityClass = "BelowNormal" } catch {}
Remove-Item Env:XBE_TEST_HIDDEN -ErrorAction SilentlyContinue
Set-Content $pidFile $p.Id
# the game reads the cmd file ten times a second and has it open for a moment: a write that fails is tried again
function Send-Cmd([string]$text) {
    for ($try = 0; $try -lt 20; $try++) {
        try { Set-Content (Join-Path $rel "cmd") $text -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 30 }
    }
    "could not write the cmd file: $text"
}
$start = Get-Date
$list = @($Captures -split "," | Where-Object { $_ -ne "" } | ForEach-Object { [double]$_ })
if ($Every -gt 0) { for ($t = [Math]::Max($From, $Every); $t -lt $Seconds - 0.5; $t += $Every) { $list += $t } }
$todo = @($list | Sort-Object)
while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    if ($p.HasExited) { "game exited after {0:N1} s, code {1}" -f ((Get-Date) - $start).TotalSeconds, $p.ExitCode; break }
    $t = ((Get-Date) - $start).TotalSeconds
    if ($todo.Count -gt 0 -and $t -ge $todo[0]) {
        Send-Cmd "CAPTURE"
        if ($todo.Count -gt 1) { $todo = $todo[1..($todo.Count - 1)] } else { $todo = @() }
    }
    if ($shotTimes.Count -gt 0 -and $t -ge $shotTimes[0]) {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $bmp.Save((Join-Path $shotDir ("screen_{0:000}.png" -f [int]$shotTimes[0])), [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        if ($shotTimes.Count -gt 1) { $shotTimes = $shotTimes[1..($shotTimes.Count - 1)] } else { $shotTimes = @() }
    }
    Start-Sleep -Milliseconds 250
}
if (-not $p.HasExited -and -not $Keep) { Start-Sleep -Seconds 1; Stop-Process -Id $p.Id -Force; "stopped after $Seconds s (still running: good)" }
if (-not $Keep -and (Test-Path $kept)) { Start-Sleep -Milliseconds 300; Move-Item $kept $settings -Force }
Start-Sleep -Milliseconds 500
if ($Name -ne "") {
    $dst = Join-Path $proj ("runs\" + $Name)
    New-Item -ItemType Directory -Force $dst | Out-Null
    Get-ChildItem $logdir -File -Filter "suffering2*.log" -ErrorAction SilentlyContinue | Where-Object { $logsBefore -notcontains $_.Name } | Copy-Item -Destination (Join-Path $dst "log.txt") -Force
    foreach ($c in Get-ChildItem $cap -Directory) {   # each capture's picture, named by its folder (the time of day)
        $f = Join-Path $c.FullName "final.bmp"
        if (Test-Path $f) { Copy-Item $f (Join-Path $dst ($c.Name + ".bmp")) -Force }
    }
    if ($Audio -and (Test-Path (Join-Path $rel "audio.wav"))) { Copy-Item (Join-Path $rel "audio.wav") $dst -Force }
    "saved to $dst"
}
"captures: " + (Get-ChildItem $cap -Directory).Count
$log = Get-ChildItem $logdir -File -Filter "suffering2*.log" -ErrorAction SilentlyContinue | Where-Object { $logsBefore -notcontains $_.Name } | Select-Object -Last 1
if ($log) { "log: $($log.FullName) ($($log.Length) bytes)"; Get-Content $log.FullName -Tail $Tail | ForEach-Object { if ($_.Length -gt 260) { $_.Substring(0, 260) } else { $_ } } }
# a freeze the game reported itself (src\game\watchdog.cpp): say so, whatever -Tail is
if ($log) {
    $freeze = @(Select-String -Path $log.FullName -Pattern "freeze report: (no frame|frames again)")
    if ($freeze.Count) { "FREEZE REPORTED in $($log.Name): " + (($freeze | ForEach-Object { ($_.Line -replace '^.*freeze report: ', '') }) -join "; ") }
}
