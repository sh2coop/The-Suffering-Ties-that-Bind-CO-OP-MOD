# Makes a release: the next version number, the hand-out folder, the two zips, the signature, and (with -Publish)
# the source pushed to the project's repository and a GitHub release that the folder's "Update" program reads.
#   powershell -File tools\release.ps1 -Note "what changed" [-Publish] [-NoBuild]
#
#   version.txt              "<number> <date> <note>"   what Update compares with the folder's VERSION.txt
#   update.zip               every file of the folder that can change (all but clang.exe and lld-link.exe)
#   update.zip.sig           its signature with the release key (never in the repository: -Key)
#   The-Suffering-2-PC.zip   the whole folder, for a first install
# No game data is in any of it. What goes to the repository is this project's tracked files as one commit on top of
# the repository's history (the working history, with its notes about people and machines, stays here).
param(
    [Parameter(Mandatory = $true)][string]$Note,
    [switch]$Publish,
    [switch]$NoBuild,
    [string]$Repo = "sh2coop/The-Suffering-Ties-that-Bind-CO-OP-MOD",
    [string]$Key = ""
)
$ErrorActionPreference = "Stop"
$proj = Split-Path -Parent $PSScriptRoot
$xbox = Split-Path -Parent $proj
if (-not $Key) { $Key = Join-Path $xbox "release signing key - KEEP PRIVATE, never upload\suffering2-update-private.pem" }
if (-not (Test-Path $Key)) { throw "the release key is missing: $Key" }
$openssl = "C:\Program Files\Git\mingw64\bin\openssl.exe"
if (-not (Test-Path $openssl)) { $openssl = (Get-Command openssl -ErrorAction Stop).Source }
$tar = "$env:SystemRoot\System32\tar.exe"

# the next number
$versionFile = Join-Path $proj "VERSION.txt"
$number = 0
if (Test-Path $versionFile) { $number = [int]((Get-Content $versionFile -First 1) -split " ")[0] }
$number++
$line = "$number $(Get-Date -Format yyyy-MM-dd) $Note"
Set-Content -Encoding ascii $versionFile $line
"version: $line"

# the folder
$out = Join-Path $xbox "release\The Suffering 2 PC"
$args2 = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "package.ps1"))
if ($NoBuild) { $args2 += "-NoBuild" }
& powershell @args2 | Select-Object -Last 1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$out\Update.exe")) { throw "packaging failed" }

# the release's files
$rel = Join-Path $xbox "release\v$number"
if (Test-Path $rel) { Remove-Item $rel -Recurse -Force }
New-Item -ItemType Directory $rel | Out-Null
Set-Content -Encoding ascii "$rel\version.txt" $line
Push-Location $out
& $tar -a -cf "$rel\update.zip" --exclude "Game Files/compiler/clang.exe" --exclude "Game Files/compiler/lld-link.exe" "Build Recomp.exe" "The Suffering 2.exe" "Update.exe" "READ ME.txt" "VERSION.txt" "Game Files"
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "update.zip was not made" }
Pop-Location
Push-Location (Split-Path -Parent $out)
& $tar -a -cf "$rel\The-Suffering-2-PC.zip" "The Suffering 2 PC"
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "the full zip was not made" }
Pop-Location
& $openssl dgst -sha256 -sign $Key -out "$rel\update.zip.sig" "$rel\update.zip"
if ($LASTEXITCODE -ne 0) { throw "signing failed" }
Get-ChildItem $rel | ForEach-Object { "  {0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB) }
if (-not $Publish) { "not published (no -Publish): $rel"; exit 0 }

# the source: the tracked files of this commit, as one commit in a clone of the repository
$ErrorActionPreference = "Continue"   # git writes its remarks to the error stream; each step is checked by hand
& git -C $proj add -A VERSION.txt
& git -C $proj commit -q -m "Version $number" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>" 2>$null
$clone = Join-Path $xbox "release\repo (working copy - do not edit)"
if (-not (Test-Path "$clone\.git")) {
    & git clone -q "https://github.com/$Repo.git" $clone 2>$null
    if (-not (Test-Path "$clone\.git")) { throw "the repository could not be cloned" }
}
# The repository's main branch IS the playable folder, so that the green Code button's "Download ZIP" gives a folder
# that works as it is (asked for on 2026-11-25): the hand-out folder at the top, the source under "source".
# clang.exe is over GitHub's 100 MB limit for one file, so the two big compiler programs go in zipped (42 and 28 MB)
# and Build Recomp unpacks them. The zips are made once and kept: a new zip of the same program would differ in its
# dates and add 70 MB to the repository's history at every release.
$keep = Join-Path $rel "keep"
New-Item -ItemType Directory -Force $keep | Out-Null
foreach ($tool in "clang", "lld-link") {
    $have = Join-Path $clone "Game Files\compiler\$tool.zip"
    if (Test-Path $have) { Move-Item $have $keep }
    else {
        Push-Location (Join-Path $out "Game Files\compiler")
        & $tar -a -cf (Join-Path $keep "$tool.zip") "$tool.exe"
        Pop-Location
    }
}
Get-ChildItem $clone -Force | Where-Object { $_.Name -ne ".git" } | Remove-Item -Recurse -Force
& robocopy $out $clone /E /XF clang.exe lld-link.exe /NFL /NDL /NJH /NJS /NP | Out-Null
foreach ($tool in "clang", "lld-link") { Move-Item (Join-Path $keep "$tool.zip") (Join-Path $clone "Game Files\compiler") }
Remove-Item $keep
# what Update fetches: this version's zip and its signature, under a name with the number in it
$signed = Join-Path $clone "Game Files\signed-update"
New-Item -ItemType Directory -Force $signed | Out-Null
Copy-Item "$rel\update.zip" (Join-Path $signed "update-$number.zip")
Copy-Item "$rel\update.zip.sig" (Join-Path $signed "update-$number.zip.sig")
Set-Content -Encoding ascii (Join-Path $clone "Logs\what is this folder.txt") "The game writes a log file here every time it is started, and its crash reports. For the three newest starts there is also a packed record of every frame (..._detail.log.gz) and a folder of small pictures of the game's window, one every three seconds (..._pictures).`r`nSaw something wrong on the screen? Press F9 while it is there: a full picture and the game's state are saved here.`r`nIf something goes wrong, zip this folder and send it."
$source = Join-Path $clone "source"
New-Item -ItemType Directory $source | Out-Null
$archive = Join-Path $rel "source.tar"
& git -C $proj archive --format=tar -o $archive HEAD
& $tar -xf $archive -C $source
Remove-Item $archive
Move-Item (Join-Path $source "README.md") $clone
Copy-Item (Join-Path $source "LICENSE") $clone
# nothing is to be changed on the way in or out, and what a player's folder gains is never committed
Set-Content -Encoding ascii (Join-Path $clone ".gitattributes") "* -text"
Set-Content -Encoding ascii (Join-Path $clone ".gitignore") @"
/drop xbox iso here/*
!/drop xbox iso here/PUT YOUR GAME IMAGE HERE.txt
/Logs/*
!/Logs/what is this folder.txt
/Game Files/assets/
/Game Files/Suffering2.dll
/Game Files/compiler/work/
/Game Files/compiler/clang.exe
/Game Files/compiler/lld-link.exe
/Game Files/hdd/
/Game Files/shader_cache/
/Game Files/update/
/Game Files/settings.toml
/Update.old.exe
"@
& git -C $clone add -A
& git -C $clone -c user.name="sh2coop" -c user.email="sh2coop@users.noreply.github.com" commit -q -m "Version $number`: $Note" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
& git -C $clone branch -M main
& git -C $clone push -q -u origin main
if ($LASTEXITCODE -ne 0) { throw "the push failed" }
"published: https://github.com/$Repo (main is version $number)"
