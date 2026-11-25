# Stops the game a test run started (its process id is in out\build\release\test.pid). A game the player
# started is never touched. The process is opened by its id (tools\procs.py): the system's process list is not
# walked, because a game stuck in the kernel can make that hang.
$rel = Join-Path (Split-Path -Parent $PSScriptRoot) "out\build\release"
$pidFile = Join-Path $rel "test.pid"
if (-not (Test-Path $pidFile)) { "no test game"; exit 0 }
$mine = [int](Get-Content $pidFile)
python (Join-Path $PSScriptRoot "procs.py") --stop $mine
$kept = Join-Path $rel "settings.player.toml"
if (Test-Path $kept) { Start-Sleep -Milliseconds 300; Move-Item $kept (Join-Path $rel "settings.toml") -Force }
