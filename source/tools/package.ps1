# Makes the folder that is handed out: no game data in it, the player adds his own Xbox disc image.
#   powershell -File tools\package.ps1 [-Out <folder>] [-NoBuild]
#
#   The Suffering 2 PC\
#     The Suffering 2.exe     the one program to start (tools\launcher\launcher.c), with the game's icon
#     READ ME.txt
#     drop xbox iso here\                the player's image goes here
#     Game Files\             Suffering2.exe, the compiler folder (the first start builds the game's code with it,
#                             from the player's image), the Visual C++ runtime, an empty assets folder
param([string]$Out = "", [switch]$NoBuild)
$ErrorActionPreference = "Stop"
$proj = Split-Path -Parent $PSScriptRoot
$rel = Join-Path $proj "out\build\release"
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $proj) "release\The Suffering 2 PC" }

if (-not $NoBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1") | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { throw "the build failed" }
}
if (-not (Test-Path "$rel\Suffering2.exe")) { throw "no build: $rel\Suffering2.exe" }

# the Visual C++ runtime beside the executables, so a fresh Windows needs nothing more
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC" -Directory | Where-Object { $_.Name -match '^1' } | Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem "$($_.FullName)\x64" -Directory -Filter "Microsoft.VC*.CRT" } | Select-Object -First 1
if (-not $crt -or -not (Test-Path "$($crt.FullName)\vcruntime140.dll")) { throw "the Visual C++ runtime was not found" }

if (Test-Path $Out) {
    # never through a link into someone's game image: links are taken away first, then the folder
    Get-ChildItem $Out -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue | ForEach-Object { [IO.Directory]::Delete($_.FullName) }
    Remove-Item $Out -Recurse -Force
}
$files = Join-Path $Out "Game Files"
New-Item -ItemType Directory -Force "$files\assets", "$files\compiler", (Join-Path $Out "drop xbox iso here"), (Join-Path $Out "Logs") | Out-Null

Copy-Item "$rel\Suffering2.exe" $files
foreach ($d in "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") { Copy-Item (Join-Path $crt.FullName $d) $files }
# the compiler folder without its work folder (that is game code) and without debug files
& robocopy "$rel\compiler" "$files\compiler" /E /XD work /XF *.pdb *.dll /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "copying the compiler folder failed" }
foreach ($d in "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") { Copy-Item (Join-Path $crt.FullName $d) "$files\compiler" }
$sdk = (Get-Content "$rel\sdk_dir.txt" -ErrorAction SilentlyContinue | Select-Object -First 1)
if ($sdk -and (Test-Path "$sdk\THIRD_PARTY.txt")) { Copy-Item "$sdk\THIRD_PARTY.txt" $files }

# the launcher, with the icon
$tmp = Join-Path $rel "launcher_build"
New-Item -ItemType Directory -Force $tmp | Out-Null
$dev = Join-Path $PSScriptRoot "dev.bat"
$src = Join-Path $PSScriptRoot "launcher"
# (a batch file with paths relative to the project, where dev.bat puts the tools: no quotes inside quotes for cmd)
$bat = Join-Path $tmp "make.bat"
@"
@echo off
call "$dev" llvm-rc /fo out\build\release\launcher_build\launcher.res tools\launcher\suffering2.rc || exit /b 1
call "$dev" clang-cl /nologo /O2 /MT /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS tools\launcher\launcher.c out\build\release\launcher_build\launcher.res /Foout\build\release\launcher_build\ /Feout\build\release\launcher_build\launcher.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib winhttp.lib bcrypt.lib crypt32.lib || exit /b 1
"@ | Set-Content -Encoding ascii $bat
Remove-Item "$tmp\launcher.exe" -ErrorAction SilentlyContinue
cmd /c "`"$bat`" > `"$tmp\make.log`" 2>&1"
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$tmp\launcher.exe")) { Get-Content "$tmp\make.log"; throw "the launcher did not compile" }
Copy-Item "$tmp\launcher.exe" "$Out\The Suffering 2.exe"
Copy-Item "$tmp\launcher.exe" "$Out\Build Recomp.exe"   # the same program: its name makes it the builder
Copy-Item "$tmp\launcher.exe" "$Out\Update.exe"         # and this name makes it the updater (tools\launcher\update.h)
# the version this folder is: a number that only goes up, and a line for people (tools\release.ps1 sets it)
$version = Get-Content (Join-Path $proj "VERSION.txt") -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $version) { $version = "0 development build" }
Set-Content -Encoding ascii (Join-Path $Out "VERSION.txt") $version

@"
Put your own Xbox disc image of The Suffering: Ties That Bind here
(the USA / Europe release; a file ending in .iso).

If you have it as an archive (.7z, .zip, .rar), unpack it first.
Then double-click "Build Recomp" in the folder above.
"@ | Set-Content -Encoding utf8 (Join-Path $Out "drop xbox iso here\PUT YOUR GAME IMAGE HERE.txt")

@"
THE SUFFERING: TIES THAT BIND on PC, with co-op (test build)

HOW TO START

  1. Put your Xbox disc image of the game (the .iso file) into the folder "drop xbox iso here".
  2. Double-click "Build Recomp".
  3. Wait while it builds the game (a few minutes, only this once).
     When it says it is ready, press Enter. The game starts.

That is all. From then on, double-click "The Suffering 2" to play.

GOOD TO KNOW

  - Your image is a .7z, .zip or .rar? Unpack it first, the .iso is inside.
  - Windows asks once if the game may use the network. Say Allow if you want to play online.
  - No game data comes with this folder. You need your own copy of the game.

CO-OP

  On the main menu: "Co-op" opens "Play with a Friend". (In a game: Options, "Display and Co-op".)
  - Split screen: two controllers on one PC. Controller 1 is Player 1.
  - Online, no port forwarding needed:
      1. The host picks "Invite a friend". His screen shows a short code such as WOLF-4271.
         He sends it to his friend (X or Ctrl+C copies it).
      2. The friend picks "Join a friend" and enters the code (Y or Ctrl+V pastes it).
         He is in. While the host is still in his menus the friend watches them.
    The host's PC runs the game and sends the friend his picture and sound. The picture is as
    sharp as the friend's window, up to 1080 rows, and gets smaller by itself when the connection
    loses pictures. The limit is "friend_picture_rows" in Game Files\settings.toml on the host's
    PC (480 to 2160). A picture of 1080 rows needs about 20 to 40 Mbit/s of the host's upload.
    The short code comes from a small meeting point on the internet. When it cannot be reached
    the screens offer address codes instead: each player sends the other his code. If a screen
    says the router gives every connection a new port, forward UDP port 27025 on the host's
    router and enter the plain address.

  - Player 2 stuck or left behind? Hold A+B+X+Y for five seconds (keyboard: T) and he is beside Player 1.
  - Either player can pause. Leaving an online game: hold BACK (keyboard: Backspace) for two seconds.

KEYBOARD

  Arrows = d-pad, Enter = START, Backspace = BACK, Space or J = A, K = B, U = X, I = Y,
  O = white, P = black, W A S D = move, Q and E = triggers, numpad 8 2 4 6 = look.

UPDATES

  Double-click "Update". It fetches the newest version when there is one.
  The version you have is in VERSION.txt.

SOMETHING WENT WRONG?

  Saw something wrong on the screen? Press F9 while it is there. The game saves a
  full picture of its window and what it was doing at that moment.

  Then zip the "Logs" folder (it is right here, beside "The Suffering 2") and send it.
  For every time the game was started it has a log file, and for the three newest
  starts also a packed record of every frame ("..._detail.log.gz") and a folder of
  small pictures of the game's window, one every three seconds ("..._pictures").
  Only the game's own window is ever pictured, and the keyboard is not recorded.
  An hour of play is about 60 to 100 MB. The crash reports are in there too.
"@ | Set-Content -Encoding utf8 (Join-Path $Out "READ ME.txt")

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"packaged: $Out ({0:N0} MB, {1} files)" -f $size, (Get-ChildItem $Out -Recurse -File).Count
