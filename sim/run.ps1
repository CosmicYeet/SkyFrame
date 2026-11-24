# Build the SkyFrame simulator (only what changed) and start it.
#   .\sim\run.ps1                 radar centered on SkyFrame's default location
#   .\sim\run.ps1 39.18 -76.67    radar centered on these coordinates
param([string]$Latitude, [string]$Longitude)
$ErrorActionPreference = "Stop"
$msys = "C:\msys64\ucrt64\bin"
if (-not (Test-Path "$msys\gcc.exe")) { throw "MSYS2 UCRT64 not found at $msys (install MSYS2 and gcc, SDL2, curl, cmake, ninja)." }
$env:PATH = "$msys;$env:PATH"
$sim = $PSScriptRoot
$out = Join-Path $sim "out"
if (-not (Test-Path "$out\build.ninja")) {
    & "$msys\cmake.exe" -S $sim -B $out -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
& "$msys\cmake.exe" --build $out
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$args = @()
if ($Latitude -and $Longitude) { $args = @($Latitude, $Longitude) }
& "$out\skyframe_sim.exe" @args
