# Publish a SkyFrame release that devices install by themselves overnight.
#
#   .\tools\release.ps1 0.5.1 "Smoother radar and a new Today's sky"
#
# 1. Checks: a clean, up-to-date main branch, a newer version, the signing key next to the
#    project folder (..\SkyFrame-signing-key.pem).
# 2. Runs the unit tests and builds the simulator and the firmware (signed by the build), and
#    verifies the signature.
# 3. Commits version.txt ("Release v0.5.1"), tags v0.5.1, pushes both.
# 4. Creates the GitHub release with SkyFrame.bin and latest.json, which every SkyFrame reads
#    at night: https://github.com/CosmicYeet/SkyFrame/releases/latest/download/latest.json
#
# Keep the notes short (about 40 characters): they are the line on the "Updated" card.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Notes
)
# Tools write progress to stderr: judge each step by its exit code, not by stderr output.
$ErrorActionPreference = "Continue"
$repo = "CosmicYeet/SkyFrame"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Check($ok, $text) { if (-not $ok) { throw $text } }
function Utf8File($path, $text) { [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false)) }

Step "Checks"
Check ($Version -match '^\d+\.\d+\.\d+$') "Version must look like 0.5.1"
$current = (Get-Content version.txt -TotalCount 1).Trim()
$tags = @(git tag --list "v*")
Check ([version]$Version -ge [version]$current) "v$Version is older than the current v$current"
Check (-not ($tags -contains "v$Version")) "v$Version was already released"
if ($Notes.Length -gt 45) { Write-Warning "Notes are $($Notes.Length) characters; the Updated card shows about 40." }
$key = Join-Path (Split-Path $root -Parent) "SkyFrame-signing-key.pem"
Check (Test-Path $key) "Signing key not found at $key (devices only accept firmware signed with it)"
Check ((git rev-parse --abbrev-ref HEAD) -eq "main") "Release from the main branch"
Check (-not (git status --porcelain)) "Commit or discard the changes first (git status)"
git fetch --quiet origin main
Check ((git rev-list --count "HEAD..origin/main") -eq "0") "main is behind GitHub: pull first"
Check (Get-Command gh -ErrorAction SilentlyContinue) "GitHub CLI (gh) not found"

Step "Version $current -> $Version"
Utf8File (Join-Path $root "version.txt") "$Version`n"

$cleanPath = $env:PATH
try {
    Step "Unit tests"
    $env:MSYSTEM = "UCRT64"
    $msysRoot = "/" + $root.Substring(0, 1).ToLower() + $root.Substring(2).Replace('\', '/')
    & C:\msys64\usr\bin\bash.exe -lc "cd '$msysRoot' && sh tests/run_tests.sh"
    Check ($LASTEXITCODE -eq 0) "Unit tests failed"

    Step "Simulator build"
    $env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
    if (-not (Test-Path "sim\out\build.ninja")) { & cmake -S sim -B sim\out -G Ninja -DCMAKE_BUILD_TYPE=Release | Out-Null }
    & cmake --build sim\out
    Check ($LASTEXITCODE -eq 0) "Simulator build failed"
    # ESP-IDF refuses to run inside an MSYS2 environment: put the shell back as it was.
    Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
    $env:PATH = $cleanPath

    Step "Firmware build (signed)"
    . C:\Espressif\tools\Microsoft.v6.1.1.PowerShell_profile.ps1 *> $null
    & idf.py build
    Check ($LASTEXITCODE -eq 0) "Firmware build failed"
    & espsecure verify-signature --version 2 --keyfile $key build\SkyFrame.bin
    Check ($LASTEXITCODE -eq 0) "The firmware's signature doesn't verify"
} catch {
    Utf8File (Join-Path $root "version.txt") "$current`n"   # nothing published: put the version back
    throw
}

Step "Release files"
$bin = Join-Path $root "build\SkyFrame.bin"
$manifest = Join-Path $root "build\latest.json"
$info = [ordered]@{
    version = $Version
    url     = "https://github.com/$repo/releases/download/v$Version/SkyFrame.bin"
    notes   = $Notes
    size    = (Get-Item $bin).Length
}
Utf8File $manifest ($info | ConvertTo-Json)
Get-Content $manifest

Step "Commit, tag, push"
if (git status --porcelain version.txt) {   # unchanged for a first release of the current version
    git add version.txt
    git commit --quiet -m "Release v$Version" -m $Notes
    Check ($LASTEXITCODE -eq 0) "Commit failed"
}
git tag -a "v$Version" -m "SkyFrame v$Version"
Check ($LASTEXITCODE -eq 0) "Tagging failed"
git push --quiet origin main
Check ($LASTEXITCODE -eq 0) "Push failed"
git push --quiet origin "v$Version"
Check ($LASTEXITCODE -eq 0) "Pushing the tag failed"

Step "GitHub release"
gh release create "v$Version" $bin $manifest --repo $repo --title "SkyFrame v$Version" --notes $Notes --latest
Check ($LASTEXITCODE -eq 0) "Creating the GitHub release failed (the tag is pushed: retry with gh release create)"
Write-Host "`nReleased v$Version. Devices install it in the 3 AM hour, or now from test mode > Check for updates." -ForegroundColor Green
