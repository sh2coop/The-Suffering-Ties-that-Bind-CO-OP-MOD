# Logs dwords of the running game's memory: powershell -File tools\peek.ps1 384BC8 4 [more "addr count" pairs]
# Each pair is written to the cmd file in turn; the answers are the "peek" lines of the log.
$rel = Join-Path (Split-Path -Parent $PSScriptRoot) "out\build\release"
for ($i = 0; $i -lt $args.Count; $i += 2) {
    $n = if ($i + 1 -lt $args.Count) { $args[$i + 1] } else { "8" }
    for ($try = 0; $try -lt 20; $try++) {
        try { Set-Content (Join-Path $rel "cmd") ("PEEK " + $args[$i] + " " + $n) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 30 }
    }
    Start-Sleep -Milliseconds 350
}
Start-Sleep -Milliseconds 300
$log = Get-ChildItem (Join-Path $rel "logs") -File | Sort-Object CreationTime | Select-Object -Last 1
Select-String -Path $log.FullName -Pattern "peek " | Select-Object -Last ([int]($args.Count / 2 + 0.5)) | ForEach-Object { $_.Line -replace '^.*peek ', '' }
