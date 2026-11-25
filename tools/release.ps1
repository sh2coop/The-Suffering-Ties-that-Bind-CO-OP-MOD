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
Get-ChildItem $clone -Force | Where-Object { $_.Name -ne ".git" } | Remove-Item -Recurse -Force
$archive = Join-Path $rel "source.tar"
& git -C $proj archive --format=tar -o $archive HEAD
& $tar -xf $archive -C $clone
Remove-Item $archive
& git -C $clone add -A
& git -C $clone -c user.name="sh2coop" -c user.email="sh2coop@users.noreply.github.com" commit -q -m "Version $number`: $Note" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
& git -C $clone branch -M main
& git -C $clone push -q -u origin main
if ($LASTEXITCODE -ne 0) { throw "the push failed" }
& gh release create "v$number" "$rel\version.txt" "$rel\update.zip" "$rel\update.zip.sig" "$rel\The-Suffering-2-PC.zip" --repo $Repo --title "Version $number" --notes $Note --latest
if ($LASTEXITCODE -ne 0) { throw "the release was not created" }
"published: https://github.com/$Repo/releases/tag/v$number"
