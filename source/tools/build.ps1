# Incremental build of the exe and the game DLL (output in build2.log). Prints errors and the link lines.
#   powershell -File tools\build.ps1
$proj = Split-Path -Parent $PSScriptRoot
# the exe cannot be replaced while it runs: a game a test started is stopped, a game the player started is not
$pidFile = Join-Path $proj "out/build/release/test.pid"
$mine = if (Test-Path $pidFile) { [int](Get-Content $pidFile) } else { 0 }
foreach ($g in Get-Process Suffering2 -ErrorAction SilentlyContinue) {
    if ($g.Id -eq $mine) { Stop-Process -Id $g.Id -Force }
    else { "The game is running and no test started it (process $($g.Id)): not building."; "build exit 3"; exit 3 }
}
cmd /c "`"$proj\tools\dev.bat`" cmake --build out\build\release > `"$proj\build2.log`" 2>&1"
$code = $LASTEXITCODE
Get-Content "$proj\build2.log" | Where-Object { $_ -match 'error|FAILED|Linking|ninja: (?!Entering)' } | Select-Object -Last 25 |
    ForEach-Object { if ($_.Length -gt 300) { $_.Substring(0, 300) } else { $_ } }
# The runtime rebuilds Suffering2.dll itself when the exe is newer than the DLL (the release flow). Here ninja
# relinks the DLL whenever it has to, so an older DLL is still the right one: mark it current.
$rel = Join-Path $proj "out\build\release"
if ($code -eq 0 -and (Test-Path "$rel\Suffering2.dll")) {
    $exe = Get-Item "$rel\Suffering2.exe"; $dll = Get-Item "$rel\Suffering2.dll"
    if ($dll.LastWriteTime -le $exe.LastWriteTime) { $dll.LastWriteTime = $exe.LastWriteTime.AddSeconds(1) }
}
"build exit $code"
exit $code
